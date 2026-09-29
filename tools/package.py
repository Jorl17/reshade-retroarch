"""Builds the release zip: everything a user extracts into the game folder.

    python tools/package.py [--build out/build] [--librashader path/to/librashader.dll]
                            [--version X] [--out out/package]

Without --librashader, the pinned official librashader release is downloaded (once, into
out/cache) and checked against its SHA-256.

Zip layout (extract into the folder with the game's .exe):
    RetroArchShaders.addon64
    librashader.dll
    reshade-shaders/Shaders/RetroArchShaders.fx
    retroarch-shaders/PUT-SHADERS-HERE.txt
    RetroArchShaders-docs/  README.md, LICENSE.txt, THIRD_PARTY_NOTICES.md, licenses/
"""
import argparse
import hashlib
import io
import os
import subprocess
import sys
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LIBRASHADER_VERSION = "0.12.0"
LIBRASHADER_URL = ("https://github.com/SnowflakePowered/librashader/releases/download/"
                   f"librashader-v{LIBRASHADER_VERSION}/librashader-x86_64-windows-v{LIBRASHADER_VERSION}-optimized.zip")
LIBRASHADER_SHA256 = "521fe0f364bfa705883f9e99fb1733a3a24f0f403bcc5591b6b78f6cff289183"


def fetch_librashader(cache):
    """Returns the bytes of librashader.dll from the pinned official release."""
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, os.path.basename(LIBRASHADER_URL))
    if not os.path.exists(path):
        print(f"downloading {LIBRASHADER_URL}")
        with urllib.request.urlopen(LIBRASHADER_URL) as r, open(path + ".part", "wb") as f:
            f.write(r.read())
        os.replace(path + ".part", path)
    with open(path, "rb") as f:
        data = f.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != LIBRASHADER_SHA256:
        sys.exit(f"{path}: SHA-256 {digest} does not match the pinned {LIBRASHADER_SHA256}")
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        return z.read("librashader.dll")


def git_version():
    try:
        return subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=REPO, capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "dev"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(REPO, "out", "build"))
    ap.add_argument("--librashader")
    ap.add_argument("--version")
    ap.add_argument("--out", default=os.path.join(REPO, "out", "package"))
    args = ap.parse_args()

    addon = os.path.join(args.build, "RetroArchShaders.addon64")
    if not os.path.exists(addon):
        sys.exit(f"{addon} not found: run build.bat first")
    if args.librashader:
        with open(args.librashader, "rb") as f:
            dll = f.read()
    else:
        dll = fetch_librashader(os.path.join(REPO, "out", "cache"))

    version = args.version or git_version()
    os.makedirs(args.out, exist_ok=True)
    zip_path = os.path.join(args.out, f"RetroArchShaders-{version}.zip")
    tp = os.path.join(REPO, "third_party")
    files = [
        ("RetroArchShaders.addon64", addon),
        ("reshade-shaders/Shaders/RetroArchShaders.fx",
         os.path.join(REPO, "package", "reshade-shaders", "Shaders", "RetroArchShaders.fx")),
        ("retroarch-shaders/PUT-SHADERS-HERE.txt",
         os.path.join(REPO, "package", "retroarch-shaders", "PUT-SHADERS-HERE.txt")),
        ("RetroArchShaders-docs/README.md", os.path.join(REPO, "README.md")),
        ("RetroArchShaders-docs/LICENSE.txt", os.path.join(REPO, "LICENSE")),
        ("RetroArchShaders-docs/THIRD_PARTY_NOTICES.md", os.path.join(REPO, "THIRD_PARTY_NOTICES.md")),
        ("RetroArchShaders-docs/licenses/librashader-MPL-2.0.md", os.path.join(tp, "librashader", "LICENSE.md")),
        ("RetroArchShaders-docs/licenses/ReShade-BSD-3-Clause.md", os.path.join(tp, "reshade", "LICENSE.md")),
        ("RetroArchShaders-docs/licenses/imgui-MIT.txt", os.path.join(tp, "imgui", "LICENSE.txt")),
    ]
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, src in files:
            z.write(src, arc)
        z.writestr("librashader.dll", dll)
    print(f"{zip_path} ({os.path.getsize(zip_path) / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
