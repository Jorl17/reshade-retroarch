"""End-to-end test: the add-on inside real ReShade, on every graphics API.

    python tests/e2e.py --reshade <ReShade64.dll (any name)> --librashader <librashader.dll>
                        --shaders <folder with slang presets> --preset <preset path inside it>
                        --native <native frame .png, e.g. 424x240>
                        [--apis d3d11,d3d12,...] [--build out/build] [--work out/e2e]

The "game" is the test host (testhost/, a stand-in that shows PNG pictures through the
chosen graphics API without changing a pixel). For each API, ReShade is installed next
to it the way that API needs (d3d9.dll, d3d10.dll, d3d11.dll, dxgi.dll for D3D12,
opengl32.dll, or a Vulkan layer enabled for the test process only), with this add-on
and the TestCapture add-on, which takes screenshots and switches presets at given frame
numbers so the test never needs keyboard focus.

Scenario, the same on every API:
  1. companion .slangp present, full-frame game at 4K  -> the shader's output
  2. switch to a ReShade preset with no companion       -> frame untouched
  3. switch to another, then drop a .slangp next to it  -> untouched, then hot-reloaded (4)
  5. game switches to 4:3 pillarbox with HD side art    -> new grid, side art untouched
  6. back buffer resized to 1080p                       -> the shader's output
Separate runs: 7. 10-bit back buffer, 8. HDR10 output, 10. sRGB back buffer, and
9. librashader.dll missing. A format the host cannot draw on an API is reported as n/a.
On Direct3D 12 also 11: DirectX's shader compiler missing (untouched, no crash).

Exact runs (E1-E3), on supported APIs: the same frames with tests/presets/nearest.slangp,
a shader that enlarges the picture and inverts its colours in exact arithmetic, so its
output is the same bytes on every graphics API and never equals an untouched frame. They
must equal the offline render exactly, which proves the add-on's own work (frame copy,
native picture, write-back) is exact on that API, whatever differences librashader's
runtimes have with other presets.

"The shader's output" means: within rounding (see ROUNDING_MEAN) of an offline render of
the same frame through the same code (tools/render_png.exe) on APIs the add-on supports.
The offline render uses Direct3D 12 for the Direct3D 12 run (librashader's Direct3D 12
runtime renders some presets differently from its Direct3D 11 one) and Direct3D 11 for
the others (render_png cannot render with them). On APIs the add-on does not support yet,
every frame must be left untouched and ReShade.log must say why.

The only effect file installed is the placeholder RetroArchShaders.fx, which also
checks that the add-on runs when no other ReShade effects are present.
"""
import argparse
import json
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
ALL_APIS = ["d3d9", "d3d10", "d3d11", "d3d12", "opengl", "vulkan"]
# APIs the add-on renders on. On the others it must leave frames untouched and say why.
SUPPORTED = {"d3d9", "d3d11", "d3d12", "opengl", "vulkan"}
# What ReShade.log must say on an API the add-on does not support.
API_NAMES = {"d3d9": "Direct3D 9", "d3d10": "Direct3D 10", "d3d11": "Direct3D 11", "d3d12": "Direct3D 12",
             "opengl": "OpenGL", "vulkan": "Vulkan"}
# File name ReShade must have next to the game for each API (Vulkan uses a layer instead).
RESHADE_NAME = {"d3d9": "d3d9.dll", "d3d10": "d3d10.dll", "d3d11": "d3d11.dll", "d3d12": "dxgi.dll",
                "opengl": "opengl32.dll"}

# How a real shader preset's output is checked: within rounding of the offline render, i.e.
# an average difference of at most ROUNDING_MEAN (in 0..255 units) and at most
# ROUNDING_FRACTION of the pixels off by more than 8. Not byte for byte, because:
#  - on APIs other than Direct3D 11, librashader compiles the shaders with other compilers
#    (GLSL, DXIL, SPIR-V), so results differ by rounding;
#  - on Direct3D 11 itself, the GPU driver may not give the same bits every frame: with
#    crt-royale on an NVIDIA RTX 5070 Ti (driver 617.14), frames inside the game switch
#    between two versions a few isolated pixels apart (up to 11/255 in one channel), while
#    the same run on an AMD GPU, and offline on the NVIDIA, gives identical frames (see
#    docs/assumptions.md).
# The add-on's own work is proven exact separately: the exact runs (E1-E3) and the checks
# that frames stay untouched must match byte for byte on every API.
ROUNDING_MEAN = 0.5
ROUNDING_FRACTION = 1e-3


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(int)


def compare(a, b):
    """Compares two PNGs byte for byte. Returns (ok, description)."""
    A, B = load(a), load(b)
    if A.shape != B.shape:
        return False, f"size {A.shape[1]}x{A.shape[0]} vs {B.shape[1]}x{B.shape[0]}"
    n = int((A != B).any(2).sum())
    return n == 0, f"{n} px differ"


def within_rounding(a, b):
    """Compares two PNGs by the rule above ROUNDING_MEAN. Returns (ok, description)."""
    A, B = load(a), load(b)
    if A.shape != B.shape:
        return False, f"size {A.shape[1]}x{A.shape[0]} vs {B.shape[1]}x{B.shape[0]}"
    d = np.abs(A - B)
    mean, big = float(d.mean()), float((d.max(2) > 8).mean())
    ok = mean <= ROUNDING_MEAN and big <= ROUNDING_FRACTION
    return ok, f"within rounding: mean {mean:.2f}/255, {big * 100:.3f}% of px off by >8, max {int(d.max())}"


def newest_dxc():
    """Folder of the newest Windows SDK's x64 DirectX Shader Compiler, or None."""
    root = os.path.join(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"), "Windows Kits", "10", "bin")
    if not os.path.isdir(root):
        return None
    found = sorted(d for d in os.listdir(root) if os.path.isfile(os.path.join(root, d, "x64", "dxcompiler.dll")))
    return os.path.join(root, found[-1], "x64") if found else None


def rmtree(path):
    def make_writable_and_retry(func, p, _):
        os.chmod(p, stat.S_IWRITE)
        func(p)

    if os.path.isdir(path):
        shutil.rmtree(path, onerror=make_writable_and_retry)


def prepare_common(args, common):
    """Test frames and their offline renders, shared by every API. Returns the frame names."""
    os.makedirs(common)
    native = np.asarray(Image.open(args.native).convert("RGB"))
    nh, nw = native.shape[:2]
    n43 = native[:, (nw - 320) // 2:(nw - 320) // 2 + 320] if nw >= 320 else native
    frames = {
        "full4k": ms.stretch(native, 3840, 2160),
        "pillar4k": ms.place(3840, 2160, ms.stretch(n43, 2880, 2160), 480, 0, ms.hd_art(2160, 3840, 3)),
        "full1080": ms.stretch(native, 1920, 1080),
    }
    for name, img in frames.items():
        Image.fromarray(img).save(os.path.join(common, name + ".png"))
    # The companions reference their presets by absolute path: no copy of the shader folder
    # needed. A: the preset under test. N: the exact nearest-neighbour test preset.
    with open(os.path.join(common, "A.slangp"), "w") as f:
        f.write('#reference "{}"\n'.format(os.path.join(os.path.abspath(args.shaders), args.preset).replace("\\", "/")))
    with open(os.path.join(common, "N.slangp"), "w") as f:
        f.write('#reference "{}"\n'.format(os.path.join(REPO, "tests", "presets", "nearest.slangp").replace("\\", "/")))
    for f in ["render_png.exe"]:
        shutil.copy(os.path.join(args.build, f), common)
    shutil.copy(args.librashader, os.path.join(common, "librashader.dll"))
    # Offline renders: exp_/exact_ on Direct3D 11, and exp12_/exact12_ on Direct3D 12 for the
    # Direct3D 12 run (needs the DirectX Shader Compiler).
    renders = [("A.slangp", "exp_", []), ("N.slangp", "exact_", [])]
    if "d3d12" in args.apis.split(",") and args.dxc:
        renders += [("A.slangp", "exp12_", ["--api", "d3d12", "--dxc", args.dxc]),
                    ("N.slangp", "exact12_", ["--api", "d3d12", "--dxc", args.dxc])]
    for companion, prefix, extra in renders:
        for name in frames:
            r = subprocess.run([os.path.join(common, "render_png.exe"), os.path.join(common, companion),
                                os.path.join(common, name + ".png"), os.path.join(common, prefix + name + ".png"),
                                *extra], capture_output=True, text=True, cwd=common)
            if r.returncode != 0:
                sys.exit(f"offline render of {name} with {companion} {extra} failed:\n{r.stdout}{r.stderr}")
    return list(frames)


def install(args, api, work, common):
    """Creates the API's folder: host, ReShade, add-ons, presets. Returns extra environment."""
    os.makedirs(os.path.join(work, "shots"))
    os.makedirs(os.path.join(work, "presets"))
    os.makedirs(os.path.join(work, "reshade-shaders", "Shaders"))
    for f in ["test_host.exe", "RetroArchShaders.addon64", "TestCapture.addon64"]:
        shutil.copy(os.path.join(args.build, f), work)
    shutil.copy(args.librashader, os.path.join(work, "librashader.dll"))
    shutil.copy(os.path.join(REPO, "package", "reshade-shaders", "Shaders", "RetroArchShaders.fx"),
                os.path.join(work, "reshade-shaders", "Shaders"))
    for name in ["A", "B", "C", "N"]:
        with open(os.path.join(work, "presets", name + ".ini"), "w") as f:
            f.write("Techniques=\nTechniqueSorting=\n")
    shutil.copy(os.path.join(common, "A.slangp"), os.path.join(work, "presets", "A.slangp"))
    # Direct3D 9 gets the Shader Model 3 version of the exact test shader.
    nearest = "nearest_sm3.slangp" if api == "d3d9" else "nearest.slangp"
    with open(os.path.join(work, "presets", "N.slangp"), "w") as f:
        f.write('#reference "{}"\n'.format(os.path.join(REPO, "tests", "presets", nearest).replace("\\", "/")))
    with open(os.path.join(work, "ReShade.ini"), "w") as f:
        f.write("[GENERAL]\nEffectSearchPaths=.\\reshade-shaders\\Shaders\\**\n"
                "PresetPath=.\\presets\\A.ini\nSkipLoadingDisabledEffects=0\n\n[OVERLAY]\nTutorialProgress=4\n")
    if api == "d3d12" and args.dxc:
        for dll in ["dxcompiler.dll", "dxil.dll"]:
            shutil.copy(os.path.join(args.dxc, dll), work)
    if api != "vulkan":
        shutil.copy(args.reshade, os.path.join(work, RESHADE_NAME[api]))
        return {}
    # Vulkan: ReShade is a layer. Declare it with a manifest in the test folder and enable
    # it for the test process only, through the Vulkan loader's environment variables.
    shutil.copy(args.reshade, os.path.join(work, "ReShade64.dll"))
    manifest = {"file_format_version": "1.0.0",
                "layer": {"name": "VK_LAYER_reshade", "type": "GLOBAL", "library_path": ".\\ReShade64.dll",
                          "api_version": "1.3.268", "implementation_version": "1",
                          "description": "ReShade (test)"}}
    with open(os.path.join(work, "VkLayer_reshade.json"), "w") as f:
        json.dump(manifest, f)
    return {"VK_ADD_LAYER_PATH": work, "VK_INSTANCE_LAYERS": "VK_LAYER_reshade"}


def run_api(args, api, work, common, results):
    """Runs the scenario on one API and appends (status, label, detail) to `results`."""
    env_extra = install(args, api, work, common)
    supported = api in SUPPORTED

    def w(*p):
        return os.path.join(work, *p)

    def c(*p):
        return os.path.join(common, *p)

    def expected(name):
        """The offline render `name` (exp_... or exact_...) for this API: its Direct3D 12
        version on Direct3D 12, the Direct3D 11 one elsewhere."""
        if api == "d3d12":
            name = name.replace("exp_", "exp12_", 1).replace("exact_", "exact12_", 1)
        return c(name)

    def host(frames, script, *host_args):
        env = dict(os.environ, RRA_TEST_SCRIPT=script, **env_extra)
        r = subprocess.run([w("test_host.exe"), "--api", api, "--frames", str(frames), *host_args],
                           env=env, cwd=work, capture_output=True, text=True)
        return r.returncode, r.stderr.strip()

    def add(ok, label, detail):
        results.append(("PASS" if ok else "FAIL", f"{api:6} {label}", detail))

    def na(label, detail):
        results.append(("n/a ", f"{api:6} {label}", detail))

    def check(label, shot, rendered, source, exact=False):
        """`rendered`: expected when the add-on runs; `source`: expected when it must not."""
        if not os.path.exists(shot):
            add(False, label, "no screenshot (did ReShade load?)")
            return
        if supported and exact:
            ok, detail = compare(shot, rendered)
        elif supported:
            ok, detail = within_rounding(shot, rendered)
        else:
            ok, detail = compare(shot, source)
            detail = "untouched: " + detail
        add(ok, label, detail)

    def log_has(text):
        with open(w("ReShade.log"), encoding="utf-8", errors="ignore") as f:
            return text in f.read()

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
    code, err = host(1550, script, "--image", "0:" + c("full4k.png"), "--image", "750:" + c("pillar4k.png"),
                     "--resize", "1120:1920x1080", "--image", "1120:" + c("full1080.png"))
    if code != 0:
        add(False, "main scenario", f"test host exit {code}: {err[:200]}")
        return
    shutil.copy(w("ReShade.log"), w("ReShade-run1.log"))
    check("1 companion present, 4K       ", w("shots", "1.png"), expected("exp_full4k.png"), c("full4k.png"))
    check("2 no companion                ", w("shots", "2.png"), c("full4k.png"), c("full4k.png"), exact=True)
    check("3 other preset, no companion  ", w("shots", "3.png"), c("full4k.png"), c("full4k.png"), exact=True)
    check("4 .slangp dropped in          ", w("shots", "4.png"), expected("exp_full4k.png"), c("full4k.png"))
    check("5 4:3 pillarbox, HD side art  ", w("shots", "5.png"), expected("exp_pillar4k.png"), c("pillar4k.png"))
    if os.path.exists(w("shots", "5.png")):
        s, src = load(w("shots", "5.png")), load(c("pillar4k.png"))
        ok = bool((s[:, :480] == src[:, :480]).all() and (s[:, 3360:] == src[:, 3360:]).all())
        add(ok, "5 pillarbox side art untouched", "exact" if ok else "changed")
    check("6 resized to 1080p            ", w("shots", "6.png"), expected("exp_full1080.png"), c("full1080.png"))
    if not supported:
        reason = f"{API_NAMES[api]} is not supported"
        add(log_has(reason), "  reason in ReShade.log        ", reason)

    def close(label, shot, expected):
        """10-bit frames go through 10 bits and back: a shader's output is checked within
        rounding, an untouched frame must not be off by more than 1 anywhere."""
        if not os.path.exists(shot):
            add(False, label, "no screenshot")
            return
        if supported and os.path.basename(expected).startswith("exp_"):
            ok, detail = within_rounding(shot, expected)
            add(ok, label, detail)
            return
        A, B = load(shot), load(expected)
        n = int((np.abs(A - B) > 1).any(2).sum())
        add(n == 0, label, f"{n} px differ by more than 1")

    code, err = host(300, f"250:shot={w('shots', '7.png')}", "--format", "rgb10a2", "--image", "0:" + c("full4k.png"))
    if code == 3:
        na("7 10-bit back buffer          ", f"the host cannot draw it: {err}")
    else:
        close("7 10-bit back buffer          ", w("shots", "7.png"),
              expected("exp_full4k.png") if supported else c("full4k.png"))

    code, err = host(300, f"250:shot={w('shots', '8.png')}", "--format", "rgb10a2", "--hdr10", "1",
                     "--image", "0:" + c("full4k.png"))
    if code == 3:
        na("8 HDR10 output                ", f"the host cannot draw it: {err}")
    else:
        close("8 HDR10 output untouched      ", w("shots", "8.png"), c("full4k.png"))
        reason = "HDR output is not supported" if supported else "is not supported"
        add(log_has(reason), "8 reason in ReShade.log       ", reason)

    code, err = host(300, f"250:shot={w('shots', '10.png')}", "--format", "rgba8srgb", "--image", "0:" + c("full4k.png"))
    if code == 3:
        na("10 sRGB back buffer           ", f"the host cannot draw it: {err}")
    else:
        check("10 sRGB back buffer           ", w("shots", "10.png"), expected("exp_full4k.png"), c("full4k.png"))

    if api == "d3d12" and supported:
        # Without DirectX's shader compiler: untouched, reason logged, and no crash.
        for dll in ["dxcompiler.dll", "dxil.dll"]:
            if os.path.exists(w(dll)):
                os.remove(w(dll))
        code, err = host(300, f"250:shot={w('shots', '11.png')}", "--image", "0:" + c("full4k.png"))
        add(code == 0, "11 no DirectX shader compiler  ", "ran to the end" if code == 0 else f"exit {code}")
        if code == 0:
            check("11 no DirectX shader compiler  ", w("shots", "11.png"), c("full4k.png"), c("full4k.png"), exact=True)
            add(log_has("needs dxcompiler.dll"), "11 reason in ReShade.log       ", "needs dxcompiler.dll")
        # Put the compiler back for the runs below.
        if args.dxc:
            for dll in ["dxcompiler.dll", "dxil.dll"]:
                shutil.copy(os.path.join(args.dxc, dll), work)

    if supported:
        # Exact runs with the nearest-neighbour preset (see the top of this file).
        exact_script = ";".join([f"1:preset={w('presets', 'N.ini')}", f"200:shot={w('shots', 'E1.png')}",
                                 f"600:shot={w('shots', 'E2.png')}", f"1000:shot={w('shots', 'E3.png')}"])
        code, err = host(1050, exact_script, "--image", "0:" + c("full4k.png"), "--image", "250:" + c("pillar4k.png"),
                         "--resize", "650:1920x1080", "--image", "650:" + c("full1080.png"))
        if code != 0:
            add(False, "E exact runs                  ", f"test host exit {code}: {err[:200]}")
        else:
            check("E1 exact: 4K                   ", w("shots", "E1.png"), expected("exact_full4k.png"), None, exact=True)
            check("E2 exact: pillarbox, side art  ", w("shots", "E2.png"), expected("exact_pillar4k.png"), None,
                  exact=True)
            check("E3 exact: 1080p                ", w("shots", "E3.png"), expected("exact_full1080.png"), None,
                  exact=True)

        os.remove(w("librashader.dll"))
        host(300, f"250:shot={w('shots', '9.png')}", "--image", "0:" + c("full4k.png"))
        check("9 librashader.dll missing     ", w("shots", "9.png"), c("full4k.png"), c("full4k.png"), exact=True)
        add(log_has("librashader.dll not found"), "9 reason in ReShade.log       ", "librashader.dll not found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reshade", required=True)
    ap.add_argument("--librashader", required=True)
    ap.add_argument("--shaders", required=True)
    ap.add_argument("--preset", required=True)
    ap.add_argument("--native", required=True)
    ap.add_argument("--apis", default=",".join(ALL_APIS))
    ap.add_argument("--dxc", default=newest_dxc(),
                    help="folder with dxcompiler.dll and dxil.dll, needed for Direct3D 12 (default: the Windows SDK)")
    ap.add_argument("--build", default=os.path.join(REPO, "out", "build"))
    ap.add_argument("--work", default=os.path.join(REPO, "out", "e2e"))
    args = ap.parse_args()

    work = os.path.abspath(args.work)
    # Empty the folder rather than removing it: a shell may still have it as its current folder.
    os.makedirs(work, exist_ok=True)
    for entry in os.listdir(work):
        path = os.path.join(work, entry)
        rmtree(path) if os.path.isdir(path) else os.remove(path)
    common = os.path.join(work, "common")
    prepare_common(args, common)

    results = []
    for api in args.apis.split(","):
        run_api(args, api, os.path.join(work, api), common, results)

    for status, label, detail in results:
        print(f"{status}  {label} ({detail})")
    failed = sum(1 for s, _, _ in results if s == "FAIL")
    passed = sum(1 for s, _, _ in results if s == "PASS")
    print(f"\n{passed} passed, {failed} failed, {len(results) - passed - failed} n/a")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
