#pragma once
// Reads and writes PNG files, for the test tools, the test host (testhost/) and the
// test capture add-on. The RetroArch Shaders add-on itself does not use it.
//
// It goes through the Windows Imaging Component (WIC), the image decoders and encoders
// built into Windows, so no image library is needed. WIC is a COM library, so COM must
// be initialised (CoInitializeEx) before either function is called.
// Pixels are 8-bit RGBA: 4 bytes per pixel, in the order red, green, blue, alpha.

#include <windows.h>
#include <wincodec.h>
#include <cstdint>
#include <vector>

// Link the WIC and COM libraries without having to list them in CMakeLists.txt.
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// Loads the image file `path` (PNG, or any other format WIC can read) into `rgba` as
// 8-bit RGBA, with rows packed one after the other (w * 4 bytes per row), and sets `w`
// and `h` to its size in pixels. Returns false if the file cannot be opened or decoded;
// `rgba`, `w` and `h` are then unspecified.
inline bool load_png(const wchar_t *path, std::vector<uint8_t> &rgba, UINT &w, UINT &h)
{
    IWICImagingFactory *f = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    // Open the file, take its first frame, and convert it to 32-bit RGBA whatever its
    // stored format (palette, grey, 16-bit, ...).
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&w, &h));
    if (ok)
    {
        rgba.resize(size_t(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    return ok;
}

// Saves a `w` x `h` image as the PNG file `path`, replacing the file if it exists.
// `rgba` points at the first row of 8-bit RGBA pixels and `pitch` is the number of
// bytes from the start of one row to the start of the next (at least w * 4; GPU
// textures read back to the CPU often have padding at the end of each row). The alpha
// channel is ignored: the PNG is saved fully opaque. Returns false on any failure.
inline bool save_png(const wchar_t *path, const uint8_t *rgba, UINT w, UINT h, UINT pitch)
{
    // Repack as BGRA without row padding: WIC's PNG encoder accepts 32-bit BGRA
    // directly, but not RGBA.
    std::vector<uint8_t> bgra(size_t(w) * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const uint8_t *s = rgba + size_t(y) * pitch + size_t(x) * 4;
            uint8_t *d = bgra.data() + (size_t(y) * w + x) * 4;
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 255;
        }
    IWICImagingFactory *f = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    // Create the file, a PNG encoder writing to it, and one frame. SetPixelFormat may
    // replace `fmt` with the closest format the encoder supports; the pixels are only
    // written if it kept BGRA, since that is how they are laid out.
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
              fmt == GUID_WICPixelFormat32bppBGRA &&
              SUCCEEDED(frame->WritePixels(h, w * 4, UINT(bgra.size()), bgra.data())) && SUCCEEDED(frame->Commit()) &&
              SUCCEEDED(enc->Commit());
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (f) f->Release();
    return ok;
}
