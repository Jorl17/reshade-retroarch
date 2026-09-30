"""Downloads what tests/e2e.py needs on a machine without a GPU (the CI), each at a fixed
version and checked against its SHA-256.

    python tests/ci_deps.py <folder>

What ends up in <folder>:
  ReShade64.dll                 ReShade 6.8.0 with add-on support, from its official setup
  librashader.dll               librashader 0.12.0 (the version tools/package.py ships)
  slang-shaders/                libretro's shader collection at a fixed commit; only
                                crt/zfast-crt.slangp and its shaders are checked out
  mesa/x64/                     Mesa's 64-bit drivers, among them llvmpipe, the software
                                OpenGL driver (libgallium_wgl.dll; see
                                .github/workflows/ci.yml for how CI uses it)
  downloads/                    the downloaded files

Anything already in place is not downloaded again. Needs git and 7z (7-Zip) on PATH.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import package  # noqa: E402  (tools/package.py: the pinned librashader)

RESHADE_URL = "https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe"
RESHADE_DLL_SHA256 = "0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7"  # ReShade64.dll 6.8.0.2155

SLANG_SHADERS_URL = "https://github.com/libretro/slang-shaders.git"
SLANG_SHADERS_COMMIT = "84bcd19a854348c0e6a1d3db814a76f11fb6f011"
SLANG_SHADERS_FOLDERS = ["crt/shaders/zfast_crt"]  # crt/zfast-crt.slangp comes with its parent folder

# 26.1.8 rather than 26.2.3: the software Vulkan driver of 26.2.3 corrupts the heap on
# Windows (vkcube from the Vulkan SDK crashes with it; with 26.1.8 it does not).
MESA_URL = "https://github.com/pal1000/mesa-dist-win/releases/download/26.1.8/mesa3d-26.1.8-release-msvc.7z"
MESA_SHA256 = "4c6d32e653e0ff9ad07796e40c0bcfabf2764d849e3ce4f3b1590112c87e42f9"


def sha256(path):
    """The SHA-256 of the file at `path`, in hexadecimal."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(url, folder, expected_sha256=None):
    """Downloads `url` into `folder` (unless the file is already there) and returns its
    path. Exits with an error if `expected_sha256` is given and the file does not match."""
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, url.rsplit("/", 1)[1])
    if not os.path.exists(path):
        print(f"downloading {url}", flush=True)
        # reshade.me answers 403 to Python's default User-Agent, so the requests send this
        # project's name instead.
        request = urllib.request.Request(url, headers={"User-Agent": "reshade-retroarch-tests"})
        # Written to a ".part" file first, so a broken download never has the final name.
        with urllib.request.urlopen(request) as r, open(path + ".part", "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(path + ".part", path)
    if expected_sha256 and sha256(path) != expected_sha256:
        sys.exit(f"{path}: SHA-256 {sha256(path)} does not match the pinned {expected_sha256}")
    return path


def reshade(out, downloads):
    """ReShade64.dll, taken from the zip inside ReShade's setup program."""
    dll = os.path.join(out, "ReShade64.dll")
    if not os.path.exists(dll) or sha256(dll) != RESHADE_DLL_SHA256:
        with zipfile.ZipFile(download(RESHADE_URL, downloads)) as z, open(dll, "wb") as f:
            f.write(z.read("ReShade64.dll"))
    if sha256(dll) != RESHADE_DLL_SHA256:
        sys.exit(f"{dll}: SHA-256 {sha256(dll)} does not match the pinned {RESHADE_DLL_SHA256}")


def librashader(out, downloads):
    """librashader.dll, from the release tools/package.py uses (checked there)."""
    with open(os.path.join(out, "librashader.dll"), "wb") as f:
        f.write(package.fetch_librashader(downloads))


def slang_shaders(out):
    """A checkout of SLANG_SHADERS_FOLDERS at SLANG_SHADERS_COMMIT (other files are not
    downloaded)."""
    repo = os.path.join(out, "slang-shaders")

    def git(*args):
        subprocess.run(["git", "-C", repo, *args], check=True)

    head = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if head == SLANG_SHADERS_COMMIT:
        return
    shutil.rmtree(repo, ignore_errors=True)
    subprocess.run(["git", "clone", "--filter=blob:none", "--no-checkout", "--sparse", SLANG_SHADERS_URL, repo],
                   check=True)
    git("sparse-checkout", "set", *SLANG_SHADERS_FOLDERS)
    git("checkout", "--quiet", SLANG_SHADERS_COMMIT)


def mesa(out, downloads):
    """The x64 folder of the mesa-dist-win release (Mesa's 64-bit drivers)."""
    folder = os.path.join(out, "mesa")
    if os.path.exists(os.path.join(folder, "x64", "libgallium_wgl.dll")):
        return
    archive = download(MESA_URL, downloads, MESA_SHA256)
    subprocess.run(["7z", "x", archive, f"-o{folder}", "x64", "-y"], check=True, stdout=subprocess.DEVNULL)


def main():
    """Fetches everything listed at the top of the file into the folder given."""
    out = os.path.abspath(sys.argv[1])
    downloads = os.path.join(out, "downloads")
    os.makedirs(out, exist_ok=True)
    reshade(out, downloads)
    librashader(out, downloads)
    slang_shaders(out)
    mesa(out, downloads)
    print(f"test dependencies ready in {out}")


if __name__ == "__main__":
    main()
