"""Generates grid-detection test frames with known answers.

    python make_synthetic.py <native.png> <out_dir>

Writes PNGs plus manifest.txt with one line per case:
    <valid 0|1> <native_w> <native_h> <rect_x> <rect_y> <rect_w> <rect_h> <file>
"""
import os
import sys

import numpy as np
from PIL import Image


def stretch(native, rect_w, rect_h, convention="round"):
    """Nearest-neighbour stretch. Cell i covers [edge(i), edge(i+1))."""
    nh, nw = native.shape[:2]
    if convention == "round":
        ex = np.round(np.arange(nw + 1) * rect_w / nw).astype(int)
        ey = np.round(np.arange(nh + 1) * rect_h / nh).astype(int)
    else:
        ex = (np.arange(nw + 1) * rect_w) // nw
        ey = (np.arange(nh + 1) * rect_h) // nh
    ix = np.searchsorted(ex, np.arange(rect_w), side="right") - 1
    iy = np.searchsorted(ey, np.arange(rect_h), side="right") - 1
    return native[iy][:, ix]


def sharp(native, rect_w, rect_h):
    """'Sharp bilinear': nearest inside cells, a 1 px blend at each cell edge."""
    img = stretch(native, rect_w, rect_h).astype(np.float32)
    nh, nw = native.shape[:2]
    ex = np.round(np.arange(1, nw) * rect_w / nw).astype(int)
    ey = np.round(np.arange(1, nh) * rect_h / nh).astype(int)
    out = img.copy()
    out[:, ex] = 0.5 * (img[:, ex] + img[:, ex - 1])
    out[ey, :] = 0.5 * (out[ey, :] + out[ey - 1, :])
    return out.round().astype(np.uint8)


def bilinear(native, rect_w, rect_h):
    return np.asarray(Image.fromarray(native).resize((rect_w, rect_h), Image.BILINEAR))


def hd_art(h, w, seed):
    """Smooth gradients plus fine noise: stands in for HD border art or UI."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w]
    base = np.stack([(x * 0.13 + y * 0.07) % 255, (x * 0.05 + 90) % 255, (y * 0.11 + 30) % 255], -1)
    return np.clip(base + rng.integers(-20, 20, (h, w, 3)), 0, 255).astype(np.uint8)


def place(frame_w, frame_h, img, x, y, background):
    if isinstance(background, np.ndarray):
        frame = background.copy()
    else:
        frame = np.zeros((frame_h, frame_w, 3), np.uint8)
    frame[y:y + img.shape[0], x:x + img.shape[1]] = img
    return frame


def main():
    native = np.asarray(Image.open(sys.argv[1]).convert("RGB"))
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    nh, nw = native.shape[:2]
    cases = []

    def case(name, frame, valid, nwh=(0, 0), rect=(0, 0, 0, 0)):
        Image.fromarray(frame).save(os.path.join(out, name + ".png"))
        cases.append((valid, *nwh, *rect, name + ".png"))

    # Full frame, like Origins Anniversary, at several output sizes.
    for fw, fh in [(3840, 2160), (2560, 1440), (1920, 1080)]:
        case(f"full_{nw}x{nh}_{fw}x{fh}", stretch(native, fw, fh), 1, (nw, nh), (0, 0, fw, fh))
    case(f"full_floor_{nw}x{nh}_3840x2160", stretch(native, 3840, 2160, "floor"), 1, (nw, nh), (0, 0, 3840, 2160))
    case(f"sharp_{nw}x{nh}_3840x2160", sharp(native, 3840, 2160), 1, (nw, nh), (0, 0, 3840, 2160))

    # 4:3 pillarbox (Origins Classic mode): 320x240 at 9x, HD art or black at the sides.
    n43 = native[:, (nw - 320) // 2:(nw - 320) // 2 + 320]
    img = stretch(n43, 2880, 2160)
    case("pillar43_hdart_3840x2160", place(3840, 2160, img, 480, 0, hd_art(2160, 3840, 1)), 1, (320, 240), (480, 0, 2880, 2160))
    case("pillar43_black_3840x2160", place(3840, 2160, img, 480, 0, None), 1, (320, 240), (480, 0, 2880, 2160))
    case("pillar43_sharp_black_3840x2160", place(3840, 2160, sharp(n43, 2880, 2160), 480, 0, None), 1, (320, 240), (480, 0, 2880, 2160))

    # Non-square pixels: 256x224 shown as 4:3 in 1080p (5.625 x 4.82 px per pixel).
    n224 = native[8:232, (nw - 256) // 2:(nw - 256) // 2 + 256]
    case("snes_256x224_43_1920x1080", place(1920, 1080, stretch(n224, 1440, 1080), 240, 0, None), 1, (256, 224), (240, 0, 1440, 1080))

    # Integer 4x in a larger black frame (pillar + letterbox).
    case("int4x_320x240_in_1920x1080", place(1920, 1080, stretch(n43, 1280, 960), 320, 60, None), 1, (320, 240), (320, 60, 1280, 960))

    # Must be rejected.
    case("reject_bilinear_3840x2160", bilinear(native, 3840, 2160), 0)
    case("reject_black_3840x2160", np.zeros((2160, 3840, 3), np.uint8), 0)
    case("reject_hdart_3840x2160", hd_art(2160, 3840, 2), 0)

    with open(os.path.join(out, "manifest.txt"), "w") as f:
        for c in cases:
            f.write(" ".join(str(v) for v in c) + "\n")
    print(f"{len(cases)} cases -> {out}")


if __name__ == "__main__":
    main()
