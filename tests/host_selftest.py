"""Checks that the test host shows its pictures unchanged on every graphics API.

    python tests/host_selftest.py [--build out/build] [--work out/host-selftest] [--warp]

For each API and back buffer format the host draws the same pictures, reads its own
back buffer (no ReShade involved) and the result must equal the picture byte for byte.
Combinations an API cannot provide must be reported as unsupported (exit code 3), not fail.
--warp also runs the Direct3D hosts on the software rasterizer (as on machines without
a GPU); --warp-only runs just those (for CI, which has no GPU).
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import make_synthetic as ms  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (api, format, hdr10): expected to work. Everything else in the grid must be unsupported.
SUPPORTED = {
    ("d3d9", "rgba8", False),
    ("d3d10", "rgba8", False), ("d3d10", "rgba8srgb", False), ("d3d10", "rgb10a2", False),
    ("d3d11", "rgba8", False), ("d3d11", "rgba8srgb", False), ("d3d11", "rgb10a2", False), ("d3d11", "rgb10a2", True),
    ("d3d12", "rgba8", False), ("d3d12", "rgb10a2", False), ("d3d12", "rgb10a2", True),
    ("opengl", "rgba8", False),
    ("vulkan", "rgba8", False),
}
# Depends on the display and driver (formats a Vulkan surface supports, HDR being on):
# must either work byte for byte or report unsupported.
OPTIONAL = {("vulkan", "rgba8srgb", False), ("vulkan", "rgb10a2", False), ("vulkan", "rgb10a2", True)}
APIS = ["d3d9", "d3d10", "d3d11", "d3d12", "opengl", "vulkan"]
FORMATS = [("rgba8", False), ("rgba8srgb", False), ("rgb10a2", False), ("rgb10a2", True)]
WARP_APIS = {"d3d10", "d3d11", "d3d12"}


def ramp(w, h):
    """Every byte value in every channel, in varied combinations."""
    y, x = np.mgrid[0:h, 0:w]
    return np.stack([x % 256, y % 256, (x * 7 + y * 13) % 256], -1).astype(np.uint8)


def main():
    """Makes the test pictures, runs the host once per API, format and (with --warp)
    software renderer, compares what it shows with the pictures, and prints one line per
    run. Exit code 1 if any run failed."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(REPO, "out", "build"))
    ap.add_argument("--work", default=os.path.join(REPO, "out", "host-selftest"))
    ap.add_argument("--warp", action="store_true")
    ap.add_argument("--warp-only", action="store_true")
    args = ap.parse_args()
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    host = os.path.join(args.build, "test_host.exe")

    native = ms.procedural_native()
    pictures = {
        "ramp_1080": ramp(1920, 1080),
        "game_1080": ms.stretch(native, 1920, 1080),
        "game_4k": ms.stretch(native, 3840, 2160),
        "ramp_720": ramp(1280, 720),
    }
    for name, img in pictures.items():
        Image.fromarray(img).save(os.path.join(work, name + ".png"))

    def p(name):
        return os.path.join(work, name)

    # Frames at which the host saves what it shows, and the picture expected there (None:
    # black, because the picture given at frame 30 does not fit the back buffer).
    shots = {5: "ramp_1080", 15: "game_1080", 25: "game_4k", 32: None, 40: "ramp_720"}
    script = ["--image", "0:" + p("ramp_1080.png"),
              "--image", "10:" + p("game_1080.png"),
              "--resize", "20:3840x2160", "--image", "20:" + p("game_4k.png"),
              "--image", "30:" + p("game_1080.png"),  # wrong size for the back buffer: black
              "--resize", "35:1280x720", "--image", "35:" + p("ramp_720.png")]
    sizes = {32: (3840, 2160)}  # back buffer size at the frames expected black

    runs = [] if args.warp_only else [(api, fmt, hdr, False) for api in APIS for fmt, hdr in FORMATS]
    if args.warp or args.warp_only:
        runs += [(api, fmt, hdr, True) for api in APIS if api in WARP_APIS for fmt, hdr in FORMATS]

    failed = 0
    for api, fmt, hdr, warp in runs:
        label = f"{api:6} {fmt:9} {'hdr10' if hdr else '':5} {'warp' if warp else '':4}"
        tag = f"{api}_{fmt}{'_hdr10' if hdr else ''}{'_warp' if warp else ''}"
        cmd = [host, "--api", api, "--format", fmt, "--frames", "45", "--size", "1920x1080"] + script
        if hdr:
            cmd += ["--hdr10", "1"]
        if warp:
            cmd += ["--warp", "1"]
        for f in shots:
            cmd += ["--selfshot", f"{f}:{p(f'shot_{tag}_{f}.png')}"]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        expected_ok = (api, fmt, hdr) in SUPPORTED
        optional = (api, fmt, hdr) in OPTIONAL
        if r.returncode == 3:
            ok = not expected_ok
            print(f"{'pass' if ok else 'FAIL'}  {label}  unsupported: {r.stderr.strip()}")
            failed += not ok
            continue
        if r.returncode != 0:
            print(f"FAIL  {label}  exit {r.returncode}: {(r.stderr or r.stdout).strip()[:200]}")
            failed += 1
            continue
        if not expected_ok and not optional:
            print(f"FAIL  {label}  ran, but this combination was expected to be unsupported")
            failed += 1
            continue
        problems = []
        for f, name in shots.items():
            got = np.asarray(Image.open(p(f"shot_{tag}_{f}.png")).convert("RGB"))
            if name is None:
                w, h = sizes[f]
                want = np.zeros((h, w, 3), np.uint8)
            else:
                want = pictures[name]
            if got.shape != want.shape:
                problems.append(f"frame {f}: size {got.shape[1]}x{got.shape[0]}, expected {want.shape[1]}x{want.shape[0]}")
            elif (got != want).any():
                n = int((got != want).any(2).sum())
                problems.append(f"frame {f}: {n} px differ")
        print(f"{'pass' if not problems else 'FAIL'}  {label}  " + ("; ".join(problems) or "identical"))
        failed += bool(problems)

    print(f"\n{len(runs) - failed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
