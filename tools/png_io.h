#pragma once
// Minimal PNG load/save through Windows Imaging Component (tools and tests only).

#include <windows.h>
#include <wincodec.h>
#include <cstdint>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// Loads any WIC-readable image as tightly packed RGBA8.
inline bool load_png(const wchar_t *path, std::vector<uint8_t> &rgba, UINT &w, UINT &h)
{
    IWICImagingFactory *f = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
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

// Saves RGBA8 rows (any pitch) as an opaque PNG.
inline bool save_png(const wchar_t *path, const uint8_t *rgba, UINT w, UINT h, UINT pitch)
{
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
