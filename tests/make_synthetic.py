"""Generates test frames for the grid detector, each with a known correct answer.

The add-on has to find a game's native picture (its original low resolution, e.g.
424x240) inside the high-resolution frame the game presents, in which each native pixel
has been stretched into a block of screen pixels (a "cell"). The "pixel grid" is that
native resolution plus the rectangle of the frame the picture covers. This script makes
frames the way games do (stretched pixel art, black bars, HD art around or over the
picture, menus, fades), saves them as PNGs, and writes the expected grid of each to a
manifest that tools/detect_test.cpp checks the detector against. tests/e2e.py and
tests/host_selftest.py also import its picture functions.

    python make_synthetic.py <native.png> <out_dir>
    python make_synthetic.py --procedural <out_dir>     (generated 424x240 pixel art, used in CI)

<native.png> is a native game picture to build the frames from; with --procedural one is
generated instead (and saved as native.png). The native picture must be at least 256x232
(the cases below cut 320x240 and 256x224 pieces out of the middle; 424x240 works).

Writes PNGs plus manifest.txt with one line per case:
    <valid> <native_w> <native_h> <rect_x> <rect_y> <rect_w> <rect_h> <file>
The numbers are the expected grid: native resolution, then the rectangle it covers.
<valid> is what the detector must return:
    0  no grid (a frame without sharp pixel art)
    1  exactly that grid
    2  exactly that grid, or no grid (HD menus over the game)
    3  no grid, or a piece of that grid with no bars around it (title cards)
    4  exactly that grid, and "bounded": surrounded by bars or the frame's edges
"""
import os
import sys

import numpy as np
from PIL import Image


def stretch(native, rect_w, rect_h, convention="round"):
    """Returns `native` (an H x W x 3 array) scaled to rect_w x rect_h pixels by
    nearest-neighbour: each native pixel becomes a block ("cell") of identical pixels.

    Cell i covers output pixels [edge(i), edge(i+1)), where edge(i) = i * rect / native,
    rounded to the nearest integer (convention "round") or rounded down ("floor"). When
    the scale is not a whole number the cells differ in size by one pixel, and the two
    conventions put the larger cells in different places, as different game scalers do.
    """
    nh, nw = native.shape[:2]
    if convention == "round":
        ex = np.round(np.arange(nw + 1) * rect_w / nw).astype(int)
        ey = np.round(np.arange(nh + 1) * rect_h / nh).astype(int)
    else:
        ex = (np.arange(nw + 1) * rect_w) // nw
        ey = (np.arange(nh + 1) * rect_h) // nh
    # For each output column (row), the index of the cell it falls in.
    ix = np.searchsorted(ex, np.arange(rect_w), side="right") - 1
    iy = np.searchsorted(ey, np.arange(rect_h), side="right") - 1
    return native[iy][:, ix]


def sharp(native, rect_w, rect_h):
    """Returns `native` scaled to rect_w x rect_h the way "sharp bilinear" filters do:
    like stretch(), but the first column and first row of each cell (except the first
    cell) become the average of that pixel and its neighbour in the previous cell, so
    cell edges are softened by one pixel.
    """
    img = stretch(native, rect_w, rect_h).astype(np.float32)
    nh, nw = native.shape[:2]
    ex = np.round(np.arange(1, nw) * rect_w / nw).astype(int)
    ey = np.round(np.arange(1, nh) * rect_h / nh).astype(int)
    out = img.copy()
    out[:, ex] = 0.5 * (img[:, ex] + img[:, ex - 1])
    out[ey, :] = 0.5 * (out[ey, :] + out[ey - 1, :])
    return out.round().astype(np.uint8)


def bilinear(native, rect_w, rect_h):
    """Returns `native` scaled to rect_w x rect_h with smooth (bilinear) filtering: no
    sharp cells remain, so the detector must find no grid in it.
    """
    return np.asarray(Image.fromarray(native).resize((rect_w, rect_h), Image.BILINEAR))


def hd_art(h, w, seed):
    """Returns an h x w RGB picture of smooth gradients plus fine random noise, which
    stands in for high-resolution border art or UI (no pixel grid in it). The same
    `seed` always gives the same picture.
    """
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w]
    base = np.stack([(x * 0.13 + y * 0.07) % 255, (x * 0.05 + 90) % 255, (y * 0.11 + 30) % 255], -1)
    return np.clip(base + rng.integers(-20, 20, (h, w, 3)), 0, 255).astype(np.uint8)


def procedural_native(w=424, h=240, seed=7):
    """Returns a w x h RGB pixel-art picture standing in for a game's native frame: a
    banded sky with clouds, a ground of 16x16 tiles, and sprites. The same `seed` always
    gives the same picture.
    """
    rng = np.random.default_rng(seed)
    img = np.zeros((h, w, 3), np.uint8)
    sky = np.array([[36, 72, 216], [60, 108, 232], [96, 144, 240], [144, 184, 248]], np.uint8)
    horizon = h * 5 // 8
    for y in range(horizon):
        img[y, :] = sky[min(3, y * 4 // horizon)]
    for _ in range(6):  # clouds: flat blobs with a darker edge
        cx, cy, rx, ry = rng.integers(0, w), rng.integers(8, horizon - 24), rng.integers(12, 40), rng.integers(4, 10)
        yy, xx = np.mgrid[0:h, 0:w]
        d = ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2
        img[d < 1.0] = [200, 208, 232]
        img[d < 0.6] = [248, 248, 248]
    # Ground: 16x16 tiles, each one of three random tile patterns in a 4-colour palette.
    palette = rng.integers(0, 256, (4, 3), dtype=np.uint8)
    tiles = rng.integers(0, 4, (3, 16, 16))
    for ty in range(horizon, h, 16):
        for tx in range(0, w, 16):
            t = palette[tiles[rng.integers(0, 3)]]
            img[ty:ty + 16, tx:tx + 16] = t[:h - ty, :w - tx]
    for _ in range(5):  # sprites: random 3-colour blocks with 20% of pixels left transparent
        sw, sh = rng.integers(16, 33), rng.integers(24, 41)
        x, y = rng.integers(0, w - sw), rng.integers(horizon - sh, h - sh)
        spal = rng.integers(0, 256, (3, 3), dtype=np.uint8)
        mask = rng.random((sh, sw)) < 0.8
        img[y:y + sh, x:x + sw][mask] = spal[rng.integers(0, 3, (sh, sw))][mask]
    return img


def place(frame_w, frame_h, img, x, y, background):
    """Returns a frame with picture `img` pasted with its top-left corner at (x, y).

    If `background` is an array, the frame is a copy of it (and frame_w, frame_h are not
    used); otherwise (e.g. None) the frame is black, frame_w x frame_h.
    """
    if isinstance(background, np.ndarray):
        frame = background.copy()
    else:
        frame = np.zeros((frame_h, frame_w, 3), np.uint8)
    frame[y:y + img.shape[0], x:x + img.shape[1]] = img
    return frame


def main():
    """Reads the command line (see the top of the file), writes every case's PNG into
    the output folder, then manifest.txt, and prints how many cases were written.
    """
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    if sys.argv[1] == "--procedural":
        native = procedural_native()
        Image.fromarray(native).save(os.path.join(out, "native.png"))
    else:
        native = np.asarray(Image.open(sys.argv[1]).convert("RGB"))
    nh, nw = native.shape[:2]
    cases = []

    def case(name, frame, valid, nwh=(0, 0), rect=(0, 0, 0, 0)):
        """Saves `frame` as <name>.png and records its manifest line: `valid` code,
        expected native size `nwh` and rectangle `rect` (x, y, w, h).
        """
        Image.fromarray(frame).save(os.path.join(out, name + ".png"))
        cases.append((valid, *nwh, *rect, name + ".png"))

    # The game picture fills the whole frame (like Sonic Origins' widescreen mode), at
    # several output sizes, with both edge rounding conventions and with sharp bilinear.
    for fw, fh in [(3840, 2160), (2560, 1440), (1920, 1080)]:
        case(f"full_{nw}x{nh}_{fw}x{fh}", stretch(native, fw, fh), 1, (nw, nh), (0, 0, fw, fh))
    case(f"full_floor_{nw}x{nh}_3840x2160", stretch(native, 3840, 2160, "floor"), 1, (nw, nh), (0, 0, 3840, 2160))
    case(f"sharp_{nw}x{nh}_3840x2160", sharp(native, 3840, 2160), 1, (nw, nh), (0, 0, 3840, 2160))

    # 4:3 pillarbox (Sonic Origins' Classic mode): the middle 320x240 of the picture at
    # 9x (2880x2160), centred in 4K with HD art or black at the sides.
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

    # Something drawn over a full-frame game: a pause menu, a text box, a title card.
    # The game's grid is still visible around it; the detector must return the full-frame
    # grid or no grid, never the visible piece as the whole picture.
    for fw, fh in [(3840, 2160), (2560, 1440), (1920, 1080)]:
        full = stretch(native, fw, fh)
        art = hd_art(fh, fw, 5)
        x_menu, y_bar = fw * 62 // 100, fh * 88 // 100  # HD panel on the right, HD bar at the bottom
        menu = full.copy()
        menu[:, x_menu:] = art[:, x_menu:]
        menu[y_bar:, :] = art[y_bar:, :]
        case(f"menu_over_game_{fw}x{fh}", menu, 2, (nw, nh), (0, 0, fw, fh))
        dim = (full.astype(np.uint16) * 2 // 5).astype(np.uint8)  # the same during a fade
        dim[:, x_menu:] = art[:, x_menu:]
        dim[y_bar:, :] = art[y_bar:, :]
        case(f"menu_over_dim_game_{fw}x{fh}", dim, 2, (nw, nh), (0, 0, fw, fh))
        # Sonic Origins' pause menu: a diagonal HD panel from the top right towards the
        # bottom middle, and a bar along the bottom. A clean centred box of game pixels
        # remains, which looks like a whole picture. `panel` marks the HD
        # pixels, using coordinates u, v that run from 0 to 1 across and down the frame.
        v, u = np.mgrid[0:fh, 0:fw]
        u, v = u / fw, v / fh
        panel = ((v > 0.236) & (u > 1 - (v - 0.236) * (0.375 / 0.64))) | (v > 0.878)
        for name, game in [("", full), ("dim_", (full.astype(np.uint16) * 2 // 5).astype(np.uint8))]:
            diag = game.copy()
            diag[panel] = art[panel]
            case(f"diagonal_menu_over_{name}game_{fw}x{fh}", diag, 2, (nw, nh), (0, 0, fw, fh))
        # A title card: a box of game pixels in the middle of a black screen. On its own
        # it may look like a small picture; it must never replace the full-frame grid.
        # The box is 120x40 native pixels, cut along the same cell edges stretch() uses.
        ex = np.round(np.arange(nw + 1) * fw / nw).astype(int)
        ey = np.round(np.arange(nh + 1) * fh / nh).astype(int)
        i0, i1, j0, j1 = nw // 2 - 60, nw // 2 + 60, nh // 2 - 20, nh // 2 + 20
        card = np.zeros_like(full)
        card[ey[j0]:ey[j1], ex[i0]:ex[i1]] = full[ey[j0]:ey[j1], ex[i0]:ex[i1]]
        case(f"title_card_on_black_{fw}x{fh}", card, 3, (nw, nh), (0, 0, fw, fh))

    # A genuinely smaller picture: a Mega Drive game switching from its 320-wide (H40)
    # to its 256-wide (H32) mode, at 4x with black bars. It must be detected as bounded,
    # so that it can replace a larger grid.
    n256 = native[8:232, (nw - 256) // 2:(nw - 256) // 2 + 256]
    case("h32_256x224_4x_bars_1920x1080", place(1920, 1080, stretch(n256, 1024, 896), 448, 92, None), 4,
         (256, 224), (448, 92, 1024, 896))

    # Must be rejected: a smooth upscale, a black frame, and HD art with no pixel art.
    case("reject_bilinear_3840x2160", bilinear(native, 3840, 2160), 0)
    case("reject_black_3840x2160", np.zeros((2160, 3840, 3), np.uint8), 0)
    case("reject_hdart_3840x2160", hd_art(2160, 3840, 2), 0)

    with open(os.path.join(out, "manifest.txt"), "w") as f:
        for c in cases:
            f.write(" ".join(str(v) for v in c) + "\n")
    print(f"{len(cases)} cases -> {out}")


if __name__ == "__main__":
    main()
