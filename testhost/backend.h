#pragma once
// One graphics API behind the test host. Every backend does the same thing: puts a
// test picture into the back buffer unchanged (no filtering, no shaders), presents it,
// and can read the back buffer back. So a frame is byte-identical whichever API drew
// it, and ReShade (with the add-on) sees the same input on every API.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

enum class Format
{
    rgba8,     // R8G8B8A8 (B8G8R8A8 where the API has no RGBA order)
    rgba8srgb, // the same, sRGB
    rgb10a2,   // R10G10B10A2
};

// A test picture, RGBA8, top row first.
struct Image
{
    UINT w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

struct Options
{
    HWND hwnd = nullptr;
    UINT width = 0, height = 0;
    Format format = Format::rgba8;
    bool hdr10 = false; // switch the output to HDR10 (where the API can)
    bool warp = false;  // software rendering (Direct3D only), for machines without a GPU
};

class Backend
{
public:
    virtual ~Backend() = default;
    // Returns false with `error` set. `unsupported` is set when the API cannot do what
    // was asked (a format, HDR10), as opposed to a failure.
    virtual bool init(const Options &o, std::string &error, bool &unsupported) = 0;
    virtual bool resize(UINT w, UINT h, std::string &error) = 0;
    // Fills the back buffer with `img` (same size as the back buffer), or black when null.
    virtual bool draw(const Image *img, std::string &error) = 0;
    // Reads the back buffer as RGBA8 (alpha 255), after draw and before present.
    virtual bool read_back(Image &out, std::string &error) = 0;
    virtual bool present(std::string &error) = 0;
};

std::unique_ptr<Backend> make_d3d9();
std::unique_ptr<Backend> make_d3d10();
std::unique_ptr<Backend> make_d3d11();
std::unique_ptr<Backend> make_d3d12();
std::unique_ptr<Backend> make_opengl();
std::unique_ptr<Backend> make_vulkan();

// Pixel packing shared by the backends.
inline uint32_t pack_rgb10a2(const uint8_t *p)
{
    auto c = [&](int i) { return uint32_t((p[i] * 1023u + 127u) / 255u); };
    return c(0) | c(1) << 10 | c(2) << 20 | 3u << 30;
}

// Test picture in the byte layout of a back buffer format.
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

// Back buffer bytes (rows `pitch` apart) back to RGBA8.
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
