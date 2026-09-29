"""End-to-end test: the add-on inside real ReShade, driven by test_host.

    python tests/e2e.py --reshade <ReShade d3d11.dll> --librashader <librashader.dll>
                        --shaders <folder with slang presets> --preset <preset path inside it>
                        --native <native frame .png, e.g. 424x240> [--build out/build] [--work out/e2e]

Scenario (one run of test_host, preset switches and file drops scripted by the
TestCapture add-on, so it never needs keyboard focus):
  1. companion present, full-frame game at 4K     -> matches an offline render
  2. switch to a preset with no companion         -> frame untouched
  3. switch to another preset, then drop a .slangp
     next to it while running                     -> untouched, then hot-reloaded
  4. game switches to 4:3 pillarbox with HD art   -> 320-wide grid re-detected,
                                                     side art untouched
  5. swap chain resized to 1080p                  -> re-detected, matches offline
Then separate runs:
  7. 10-bit SDR back buffer                        -> matches an offline render
  8. HDR10 output                                  -> frame untouched, reason logged
  10. sRGB back buffer (bitblt swap chain)         -> matches an offline render
  9. no librashader.dll                            -> frame untouched, reason logged

The only effect file installed is the placeholder RetroArchShaders.fx, which also
checks that the add-on runs when no other ReShade effects are present.
"""
import argparse
import os
import shutil
import stat
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import make_synthetic as ms  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Inside ReShade a few pixels of a frame can come out a little differently from an
# offline render (GPU scheduling). Anything beyond this is a real difference.
MAX_DIFF_PIXELS_FRACTION = 1e-5
MAX_DIFF_VALUE = 8


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(int)


def compare(a, b, exact):
    A, B = load(a), load(b)
    if A.shape != B.shape:
        return False, f"size {A.shape[1]}x{A.shape[0]} vs {B.shape[1]}x{B.shape[0]}"
    diff = (A != B).any(2)
    n, worst = int(diff.sum()), int(np.abs(A - B).max())
    if exact:
        return n == 0, f"{n} px differ"
    return n <= MAX_DIFF_PIXELS_FRACTION * diff.size and worst <= MAX_DIFF_VALUE, f"{n} px differ, max {worst}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reshade", required=True)
    ap.add_argument("--librashader", required=True)
    ap.add_argument("--shaders", required=True)
    ap.add_argument("--preset", required=True)
    ap.add_argument("--native", required=True)
    ap.add_argument("--build", default=os.path.join(REPO, "out", "build"))
    ap.add_argument("--work", default=os.path.join(REPO, "out", "e2e"))
    args = ap.parse_args()

    work = os.path.abspath(args.work)
    if os.path.isdir(work):
        def make_writable_and_retry(func, path, _):
            os.chmod(path, stat.S_IWRITE)  # read-only files (e.g. from an older run that copied .git)
            func(path)

        shutil.rmtree(work, onerror=make_writable_and_retry)
    os.makedirs(os.path.join(work, "shots"))
    os.makedirs(os.path.join(work, "presets"))
    os.makedirs(os.path.join(work, "reshade-shaders", "Shaders"))
    for f in ["test_host.exe", "render_png.exe", "RetroArchShaders.addon64", "TestCapture.addon64"]:
        shutil.copy(os.path.join(args.build, f), work)
    shutil.copy(args.librashader, os.path.join(work, "librashader.dll"))
    shutil.copy(args.reshade, os.path.join(work, "d3d11.dll"))
    shutil.copy(os.path.join(REPO, "package", "reshade-shaders", "Shaders", "RetroArchShaders.fx"),
                os.path.join(work, "reshade-shaders", "Shaders"))
    shutil.copytree(args.shaders, os.path.join(work, "retroarch-shaders"), ignore=shutil.ignore_patterns(".git"))

    ref = "../retroarch-shaders/" + args.preset.replace("\\", "/")
    for name in ["A", "B", "C"]:
        with open(os.path.join(work, "presets", name + ".ini"), "w") as f:
            f.write("Techniques=\nTechniqueSorting=\n")
    with open(os.path.join(work, "presets", "A.slangp"), "w") as f:
        f.write(f'#reference "{ref}"\n')
    with open(os.path.join(work, "ReShade.ini"), "w") as f:
        f.write("[GENERAL]\nEffectSearchPaths=.\\reshade-shaders\\Shaders\\**\n"
                "PresetPath=.\\presets\\A.ini\nSkipLoadingDisabledEffects=0\n\n[OVERLAY]\nTutorialProgress=4\n")

    # Test frames from the native image.
    native = np.asarray(Image.open(args.native).convert("RGB"))
    nh, nw = native.shape[:2]
    n43 = native[:, (nw - 320) // 2:(nw - 320) // 2 + 320] if nw >= 320 else native
    frames = {
        "full4k": ms.stretch(native, 3840, 2160),
        "pillar4k": ms.place(3840, 2160, ms.stretch(n43, 2880, 2160), 480, 0, ms.hd_art(2160, 3840, 3)),
        "full1080": ms.stretch(native, 1920, 1080),
    }
    for name, img in frames.items():
        Image.fromarray(img).save(os.path.join(work, name + ".png"))

    def w(*p):
        return os.path.join(work, *p)

    # Expected results, rendered offline through the same code.
    for name in frames:
        r = subprocess.run([w("render_png.exe"), w("presets", "A.slangp"), w(name + ".png"), w("exp_" + name + ".png")],
                           capture_output=True, text=True, cwd=work)
        if r.returncode != 0:
            sys.exit(f"offline render of {name} failed:\n{r.stdout}{r.stderr}")

    script = ";".join([
        f"200:shot={w('shots', '1.png')}",
        f"210:preset={w('presets', 'B.ini')}",
        f"350:shot={w('shots', '2.png')}",
        f"360:preset={w('presets', 'C.ini')}",
        f"400:shot={w('shots', '3.png')}",
        f"410:copy={w('presets', 'A.slangp')}|{w('presets', 'C.slangp')}",
        f"700:shot={w('shots', '4.png')}",
        f"710:preset={w('presets', 'A.ini')}",
        f"1100:shot={w('shots', '5.png')}",
        f"1500:shot={w('shots', '6.png')}",
    ])
    env = dict(os.environ, RRA_TEST_SCRIPT=script)
    subprocess.run([w("test_host.exe"), "--frames", "1550",
                    "--image", "0:" + w("full4k.png"),
                    "--image", "750:" + w("pillar4k.png"),
                    "--resize", "1120:1920x1080",
                    "--image", "1120:" + w("full1080.png")],
                   env=env, cwd=work, check=True, capture_output=True)
    shutil.copy(w("ReShade.log"), w("ReShade-run1.log"))

    results = []

    def check(label, shot, expected, exact=False):
        if not os.path.exists(shot):
            results.append((False, label, "no screenshot (did the add-on run?)"))
            return
        ok, detail = compare(shot, expected, exact)
        results.append((ok, label, detail))

    check("1 companion present, 4K       == offline render", w("shots", "1.png"), w("exp_full4k.png"))
    check("2 no companion                == untouched", w("shots", "2.png"), w("full4k.png"), exact=True)
    check("3 other preset, no companion  == untouched", w("shots", "3.png"), w("full4k.png"), exact=True)
    check("4 .slangp dropped in          == offline render", w("shots", "4.png"), w("exp_full4k.png"))
    check("5 4:3 pillarbox, HD side art  == offline render", w("shots", "5.png"), w("exp_pillar4k.png"))
    if os.path.exists(w("shots", "5.png")):
        s, src = load(w("shots", "5.png")), load(w("pillar4k.png"))
        ok = bool((s[:, :480] == src[:, :480]).all() and (s[:, 3360:] == src[:, 3360:]).all())
        results.append((ok, "5 pillarbox side art          == untouched", "exact" if ok else "changed"))
    check("6 resized to 1080p            == offline render", w("shots", "6.png"), w("exp_full1080.png"))

    # 10-bit back buffers: SDR gets the shader, HDR10 is left alone (and says why).
    # Values go through 10 bits and back, so allow off-by-one rounding.
    def close(shot, expected, label):
        if not os.path.exists(shot):
            results.append((False, label, "no screenshot"))
            return
        A, B = load(shot), load(expected)
        off = (np.abs(A - B) > 1).any(2)
        n = int(off.sum())
        results.append((n <= MAX_DIFF_PIXELS_FRACTION * off.size, label, f"{n} px differ by more than 1"))

    env = dict(os.environ, RRA_TEST_SCRIPT=f"250:shot={w('shots', '7.png')}")
    subprocess.run([w("test_host.exe"), "--frames", "300", "--format", "rgb10a2", "--image", "0:" + w("full4k.png")],
                   env=env, cwd=work, check=True, capture_output=True)
    close(w("shots", "7.png"), w("exp_full4k.png"), "7 10-bit SDR back buffer       == offline render")

    env = dict(os.environ, RRA_TEST_SCRIPT=f"250:shot={w('shots', '8.png')}")
    r = subprocess.run([w("test_host.exe"), "--frames", "300", "--format", "rgb10a2", "--hdr10", "1",
                        "--image", "0:" + w("full4k.png")], env=env, cwd=work, capture_output=True, text=True)
    if r.returncode == 3:
        results.append((True, "8 HDR10 output                == untouched", "skipped: HDR10 not available here"))
    else:
        close(w("shots", "8.png"), w("full4k.png"), "8 HDR10 output                == untouched")
        with open(w("ReShade.log"), encoding="utf-8", errors="ignore") as f:
            logged = "HDR output is not supported" in f.read()
        results.append((logged, "8 HDR refusal reported in ReShade.log", "yes" if logged else "no"))

    # sRGB back buffer (bitblt swap chain): the shader must see the stored values, as with UNORM.
    env = dict(os.environ, RRA_TEST_SCRIPT=f"250:shot={w('shots', '10.png')}")
    subprocess.run([w("test_host.exe"), "--frames", "300", "--format", "rgba8srgb", "--image", "0:" + w("full4k.png")],
                   env=env, cwd=work, check=True, capture_output=True)
    check("10 sRGB back buffer           == offline render", w("shots", "10.png"), w("exp_full4k.png"))

    # Last run: no librashader.dll.
    os.remove(w("librashader.dll"))
    env = dict(os.environ, RRA_TEST_SCRIPT=f"250:shot={w('shots', '9.png')}")
    subprocess.run([w("test_host.exe"), "--frames", "300", "--image", "0:" + w("full4k.png")],
                   env=env, cwd=work, check=True, capture_output=True)
    check("9 librashader.dll missing     == untouched", w("shots", "9.png"), w("full4k.png"), exact=True)
    with open(w("ReShade.log"), encoding="utf-8", errors="ignore") as f:
        log = f.read()
    logged = "librashader.dll not found" in log
    results.append((logged, "9 missing DLL reported in ReShade.log", "yes" if logged else "no"))

    for ok, label, detail in results:
        print(f"{'PASS' if ok else 'FAIL'}  {label}  ({detail})")
    failed = sum(1 for ok, _, _ in results if not ok)
    print(f"\n{len(results) - failed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
