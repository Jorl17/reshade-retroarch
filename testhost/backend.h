#pragma once
// Interface between the test host (main.cpp) and the graphics APIs it can draw with.
//
// The test host is a stand-in for a game: a program that shows pictures through
// Direct3D 9, 10, 11, 12, OpenGL or Vulkan, so ReShade and its add-ons can be tested
// on every API without a real game. Each API is implemented by one "backend" (one
// .cpp file per API). Every backend does exactly the same job:
//  - create a window's swap chain (the images shown on screen, called back buffers),
//  - copy a test picture into the current back buffer without changing any pixel
//    (no scaling, no filtering, no shaders),
//  - show it (present),
//  - on request, read the back buffer back to the CPU.
// Because nothing is transformed, the frame passed to ReShade is byte-identical on every
// API, so results from different APIs can be compared directly.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Pixel format of the back buffers.
enum class Format
{
    rgba8,     // 8 bits per channel, red first in memory (blue first on APIs that only offer that)
    rgba8srgb, // the same bytes, declared to the API as sRGB-encoded
    rgb10a2,   // 10 bits per colour channel, 2 bits of alpha, packed into 32 bits
};

// A test picture: `w` x `h` pixels, 4 bytes each (red, green, blue, alpha), top row first.
struct Image
{
    UINT w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// What a backend creates, as requested by the host.
struct Options
{
    HWND hwnd = nullptr;           // window to draw into
    UINT width = 0, height = 0;    // back buffer size in pixels
    Format format = Format::rgba8; // back buffer format
    bool hdr10 = false;            // declare the output as HDR10 (ST.2084), as games with HDR on do
    bool warp = false;             // Direct3D only: use Windows' software renderer instead of the GPU
    int adapter = -1;              // Direct3D 11 only: the GPU to use (DXGI's numbering), -1 for the default
};

// One graphics API. All functions return false on failure and put a description in
// `error`. They are called from one thread, in this order per frame:
// draw, optionally read_back, present.
class Backend
{
public:
    virtual ~Backend() = default;

    // Creates the device and swap chain described by `o`. When the API cannot provide the
    // requested format or output at all (for example Direct3D 9 has no windowed 10-bit
    // back buffers), returns false and sets `unsupported`, so callers can distinguish
    // "not possible" from "broken".
    virtual bool init(const Options &o, std::string &error, bool &unsupported) = 0;

    // Changes the back buffer size to `w` x `h`, like a game switching resolution.
    virtual bool resize(UINT w, UINT h, std::string &error) = 0;

    // Fills the current back buffer with `img`, pixel for pixel, or with black when
    // `img` is null. `img` must be the same size as the back buffer.
    virtual bool draw(const Image *img, std::string &error) = 0;

    // Copies the current back buffer to `out` as RGBA8 (alpha set to 255). Called after
    // draw and before present, so it returns exactly what draw put there.
    virtual bool read_back(Image &out, std::string &error) = 0;

    // Shows the current back buffer on screen.
    virtual bool present(std::string &error) = 0;
};

// Create the backend for each API (defined in d3d9.cpp, d3d10.cpp and so on).
std::unique_ptr<Backend> make_d3d9();
std::unique_ptr<Backend> make_d3d10();
std::unique_ptr<Backend> make_d3d11();
std::unique_ptr<Backend> make_d3d12();
std::unique_ptr<Backend> make_opengl();
std::unique_ptr<Backend> make_vulkan();

// Resizes window `hwnd` so that its client area (the part inside the border, which is
// what gets drawn into) is exactly `w` x `h` pixels, moves it to the top-left corner of
// the screen, and puts it behind every other window without giving it focus. Returns
// true if the client area really has that size afterwards.
//
// Used by the OpenGL and Vulkan backends: with those APIs on Windows the back buffer
// is always the size of the window's client area, so the only way to get, say, a
// 3840x2160 back buffer is a 3840x2160 window. Keeping it behind everything else means
// it never covers what the user is working on.
inline bool size_window(HWND hwnd, UINT w, UINT h)
{
    // Window size including the border, for the requested client area size.
    RECT r = {0, 0, LONG(w), LONG(h)};
    AdjustWindowRectEx(&r, DWORD(GetWindowLongW(hwnd, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE)));
    if (!SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE))
        return false;
    RECT c = {};
    GetClientRect(hwnd, &c);
    return UINT(c.right) == w && UINT(c.bottom) == h;
}

// Converts one RGBA8 pixel (`p`, 4 bytes) to the 32-bit R10G10B10A2 value: each 8-bit
// colour channel is scaled to 10 bits (0..255 to 0..1023, rounded), alpha is opaque.
// Red is in the lowest 10 bits, as in DXGI_FORMAT_R10G10B10A2_UNORM and
// VK_FORMAT_A2B10G10R10_UNORM_PACK32.
inline uint32_t pack_rgb10a2(const uint8_t *p)
{
    auto c = [&](int i) { return uint32_t((p[i] * 1023u + 127u) / 255u); };
    return c(0) | c(1) << 10 | c(2) << 20 | 3u << 30;
}

// Converts a test picture to the bytes a back buffer of format `f` holds: 10-bit
// packed pixels for rgb10a2, otherwise 8-bit pixels with red and blue swapped when
// `bgra` is set (for back buffers that store blue first). Rows are tightly packed.
inline std::vector<uint8_t> encode(const Image &img, Format f, bool bgra)
{
    std::vector<uint8_t> out(img.rgba.size());
    for (size_t i = 0; i < img.rgba.size(); i += 4)
    {
        const uint8_t *p = &img.rgba[i];
        if (f == Format::rgb10a2)
        {
            const uint32_t v = pack_rgb10a2(p);
            std::memcpy(&out[i], &v, 4);
        }
        else
        {
            out[i + 0] = bgra ? p[2] : p[0];
            out[i + 1] = p[1];
            out[i + 2] = bgra ? p[0] : p[2];
            out[i + 3] = 255;
        }
    }
    return out;
}

// The reverse of encode: converts back buffer bytes of format `f` into an RGBA8
// picture `out` of `w` x `h` pixels. `src` rows are `pitch` bytes apart (APIs often
// pad rows). 10-bit channels are reduced to 8 bits by dropping the 2 lowest bits,
// which gives back exactly the value encode started from.
inline void decode(const uint8_t *src, size_t pitch, UINT w, UINT h, Format f, bool bgra, Image &out)
{
    out.w = w;
    out.h = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const uint8_t *s = src + y * pitch + size_t(x) * 4;
            uint8_t *d = &out.rgba[(size_t(y) * w + x) * 4];
            if (f == Format::rgb10a2)
            {
                uint32_t v;
                std::memcpy(&v, s, 4);
                d[0] = uint8_t((v & 0x3FF) >> 2);
                d[1] = uint8_t(((v >> 10) & 0x3FF) >> 2);
                d[2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
            }
            else
            {
                d[0] = bgra ? s[2] : s[0];
                d[1] = s[1];
                d[2] = bgra ? s[0] : s[2];
            }
            d[3] = 255;
        }
}
