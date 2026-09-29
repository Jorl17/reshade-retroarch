#pragma once
// Finds a game's native picture inside the full-size frame it presents. Many games draw
// pixel art at a small "native" resolution (for example 320x224 or 424x240) and stretch it
// to fill a 1080p or 4K window; RetroArch shaders need that small picture. detect_grid()
// finds, in one frame in CPU memory, the native resolution and where the stretched picture
// sits. It does not use any graphics API: its frames come from detector.h, which
// copies them back from the GPU, and the capture shader (capture.slang) uses its result to
// rebuild the small picture on the GPU.

#include <cstddef>
#include <cstdint>
#include <string>

// Where a game's native (low resolution) image sits inside the frame it presents,
// and at what resolution. Stretching a native_w x native_h picture over a rectangle of
// the frame turns each native pixel into a block of frame pixels, called a "cell" here.
// The cells tile the rectangle like a grid, hence "pixel grid". For example, 424x240
// stretched over 3840x2160 gives cells about 9 frame pixels wide and high.
struct PixelGrid
{
    // True when the fields below describe a usable grid. When false, ignore them (a
    // rejected detection may leave its best guess in them).
    bool valid = false;
    int native_w = 0, native_h = 0;             // native resolution: size of the small picture, in its own pixels
    int rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0; // rectangle of the frame, in frame pixels, the picture was stretched over
    float match = 0.0f;                         // validation score, 0..1: share of checked pixels equal to their cell's centre pixel
    // Whether the rectangle's edges are known to be the picture's real edges. When false,
    // the grid may be only the visible part of a larger picture, for example the strip of
    // gameplay left showing beside a menu drawn over the game.
    bool bounded = false; // true if it is the whole frame, or fits the content between uniform bars (plain borders)

    // True when both grids have the same `valid`, native size and rectangle. `match` and
    // `bounded` are not compared.
    bool same_as(const PixelGrid &o) const
    {
        return valid == o.valid && native_w == o.native_w && native_h == o.native_h &&
               rect_x == o.rect_x && rect_y == o.rect_y && rect_w == o.rect_w && rect_h == o.rect_h;
    }
    // Returns a one-line summary for the user, for example
    // "424x240 in 3840x2160 at (0,0), match 100%", or "no pixel grid" when not valid.
    std::string describe() const;
};

// A read-only view of one frame in CPU memory: `height` rows of `width` pixels. `data`
// points at the top-left pixel and each row starts `pitch` bytes after the previous one
// (rows may end with padding). The view does not own or copy the memory. Detection only
// checks whether pixels are bit-for-bit equal, never what colour they are, so any 4 or 8
// byte per pixel format works.
struct FrameView
{
    const uint8_t *data = nullptr;
    int width = 0, height = 0;
    size_t pitch = 0;          // bytes per row
    int bytes_per_pixel = 4;   // 4 or 8
};

// Returns the frame coordinate of the pixel at the centre of native pixel `i` along one
// axis, when `count` native pixels are stretched over `extent` frame pixels starting at
// frame coordinate `origin`: origin + floor((i + 0.5) * extent / count), computed with
// integers so there is no rounding error. capture.slang (the GPU shader that rebuilds the
// native picture) uses the same formula, so it reads exactly the pixels detection checked.
inline int cell_centre(int origin, int extent, int count, int i)
{
    return origin + int((int64_t(2 * i + 1) * extent) / (2 * int64_t(count)));
}

// Returns true when grid `piece` is a part of grid `whole`: its rectangle lies inside
// whole's rectangle, it has the same cell size, and its cells line up with whole's cells.
// Such a piece is what detection finds when something covers the rest of the picture, such
// as a title card or a pause menu. Returns false if either grid is not valid.
bool part_of(const PixelGrid &piece, const PixelGrid &whole);

// Looks at one frame and returns where the game's native picture sits in it and at what
// resolution. It expects a picture stretched with nearest-neighbour sampling (each native
// pixel copied into a solid block of frame pixels), optionally with softened block edges
// as "sharp bilinear" filters make. Returns a grid with valid == false when the frame does
// not look like that (menus drawn at full resolution, smooth upscales, blank frames), is
// smaller than 64x64, or is not 4 or 8 bytes per pixel. If `log` is not null, it receives
// a short diagnosis for the user: the cell sizes found, the best grid, and why it was
// rejected if it was.
PixelGrid detect_grid(const FrameView &frame, std::string *log = nullptr);
