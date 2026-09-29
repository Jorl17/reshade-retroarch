#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Where a game's native (low resolution) image sits inside the frame it presents,
// and at what resolution. Many games render pixel art at e.g. 320x224 or 424x240
// and stretch it to the window themselves; RetroArch shaders need that native image.
struct PixelGrid
{
    bool valid = false;
    int native_w = 0, native_h = 0;             // native resolution
    int rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0; // area of the frame it covers
    float match = 0.0f;                         // validation score, 0..1
    bool bounded = false; // its edges are the picture's edges: the whole frame, or uniform bars around it

    bool same_as(const PixelGrid &o) const
    {
        return valid == o.valid && native_w == o.native_w && native_h == o.native_h &&
               rect_x == o.rect_x && rect_y == o.rect_y && rect_w == o.rect_w && rect_h == o.rect_h;
    }
    std::string describe() const;
};

// A CPU copy of a frame. Pixels are compared bit-for-bit, so any 4 or 8 byte
// per pixel format works.
struct FrameView
{
    const uint8_t *data = nullptr;
    int width = 0, height = 0;
    size_t pitch = 0;          // bytes per row
    int bytes_per_pixel = 4;   // 4 or 8
};

// Centre sample of native pixel `i` along an axis, in frame pixels:
// origin + floor((i + 0.5) * extent / count). Exact integer maths; the capture
// shader uses the same formula.
inline int cell_centre(int origin, int extent, int count, int i)
{
    return origin + int((int64_t(2 * i + 1) * extent) / (2 * int64_t(count)));
}

// True when `piece` lies inside `whole` on the same pixel grid (same cell size, cell
// edges lined up): a part of that picture rather than a different one.
bool part_of(const PixelGrid &piece, const PixelGrid &whole);

// Detects the native grid of a frame produced by stretching a low resolution image
// with nearest-neighbour sampling, optionally with smoothed cell edges (as "sharp"
// bilinear filters do). Returns an invalid grid when the frame does not look like
// that (HD menus, smooth upscales, blank frames). `log` receives a short diagnosis.
PixelGrid detect_grid(const FrameView &frame, std::string *log = nullptr);
