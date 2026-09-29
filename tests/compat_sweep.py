"""Runs every .slangp under a folder through render_png (the add-on's code path)
and records which ones load and render.

    python tests/compat_sweep.py <shaders folder> <input.png> <out.json> [--build out/build] [--jobs 4]
                                 [--api d3d11|d3d12|d3d9] [--dxc <folder>] [--pause-while <exe>]

The input should be a game frame (it is also used for grid detection). --api picks the
graphics API render_png renders with (Direct3D 12 needs the DirectX Shader Compiler: --dxc,
by default the newest Windows SDK's).
"""
import argparse
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e  # noqa: E402  (for newest_dxc)


def game_running(names):
    """True while any of the given processes runs (so the sweep never competes with play)."""
    if not names:
        return False
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout.lower()
    return any(f'"{n.lower()}"' in out for n in names)


def run_one(render_png, preset, image, scratch, pause_for, api_args):
    """Renders `image` with `preset` through render_png (2 frames, low priority, at most
    180 s, with the extra arguments `api_args`) and returns {"preset", "ok", "seconds",
    "error"}; the rendered picture is deleted. Waits first while a process in `pause_for`
    runs."""
    while game_running(pause_for):
        time.sleep(5)
    out = os.path.join(scratch, f"{abs(hash(preset))}.png")
    t0 = time.time()
    try:
        r = subprocess.run([render_png, preset, image, out, "--frames", "2", *api_args], capture_output=True, text=True,
                           timeout=180, creationflags=0x00004000)  # BELOW_NORMAL_PRIORITY_CLASS
        ok = r.returncode == 0
        msg = "" if ok else (r.stderr.strip() or r.stdout.strip()).splitlines()[-1][:400]
    except subprocess.TimeoutExpired:
        ok, msg = False, "timeout (180 s)"
    if os.path.exists(out):
        os.remove(out)
    return {"preset": preset, "ok": ok, "seconds": round(time.time() - t0, 2), "error": msg}


def main():
    """Finds every .slangp under the shaders folder, renders them in parallel (--jobs at a
    time) and writes the results to the JSON file."""
    ap = argparse.ArgumentParser()
    ap.add_argument("shaders")
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--build", default=os.path.join(REPO, "out", "build"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--pause-while", action="append", default=[],
                    help="process name (e.g. SonicOrigins.exe); the sweep waits while it runs")
    ap.add_argument("--api", default="d3d11", choices=["d3d11", "d3d12", "d3d9"])
    ap.add_argument("--dxc", default=e2e.newest_dxc(), help="folder with dxcompiler.dll and dxil.dll (Direct3D 12)")
    args = ap.parse_args()
    api_args = ["--api", args.api] + (["--dxc", args.dxc] if args.api == "d3d12" and args.dxc else [])

    presets = sorted(os.path.join(d, f) for d, _, fs in os.walk(args.shaders) for f in fs if f.endswith(".slangp")
                     and ".git" not in d)
    scratch = os.path.join(os.path.dirname(os.path.abspath(args.out)), "sweep-scratch")
    os.makedirs(scratch, exist_ok=True)
    render_png = os.path.join(args.build, "render_png.exe")
    results = []
    with ThreadPoolExecutor(args.jobs) as pool:
        for i, res in enumerate(pool.map(lambda p: run_one(render_png, p, args.image, scratch, args.pause_while,
                                                           api_args), presets), 1):
            results.append(res)
            if i % 100 == 0:
                print(f"{i}/{len(presets)}  failures so far: {sum(not r['ok'] for r in results)}", flush=True)
    for r in results:
        r["preset"] = os.path.relpath(r["preset"], args.shaders).replace("\\", "/")
    ok = sum(r["ok"] for r in results)
    with open(args.out, "w") as f:
        json.dump({"api": args.api, "total": len(results), "ok": ok, "results": results}, f, indent=1)
    print(f"done ({args.api}): {ok}/{len(results)} presets load and render")


if __name__ == "__main__":
    sys.exit(main())
