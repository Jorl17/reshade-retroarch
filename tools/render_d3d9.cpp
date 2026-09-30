// Implementation of render_d3d9.h: the add-on's per-frame work (FrameRenderer) with plain
// Direct3D 9, for the offline tool render_png.

#include "render_d3d9.h"
#include "capture.h"
#include "chain.h"
#include "librashader_api.h"

#include <d3d9.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

bool render_d3d9(std::vector<uint8_t> &rgba, UINT w, UINT h, const PixelGrid &grid, const std::string &preset_path,
                 const std::vector<std::pair<std::string, float>> &sets, int frames, std::string &error)
{
    // A Direct3D 9 device needs a window; this one is never shown. Its back buffer is not
    // used (everything is drawn into textures).
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"render_png", WS_POPUP, 0, 0, 16, 16,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    struct WindowGuard
    {
        HWND h;
        ~WindowGuard() { DestroyWindow(h); }
    } window_guard{hwnd};
    ComPtr<IDirect3D9> d3d;
    d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 16;
    pp.BackBufferHeight = 16;
    pp.hDeviceWindow = hwnd;
    ComPtr<IDirect3DDevice9> dev;
    // The same flags as the test host (testhost/d3d9.cpp).
    if (hwnd == nullptr || !d3d ||
        FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev)))
    {
        error = "could not create a Direct3D 9 device";
        return false;
    }

    // The frame (the add-on's snapshot: the back buffer's format, X8R8G8B8), the native
    // picture (A8R8G8B8, as FrameRenderer::native_format) and the preset's output (the
    // frame's format), in D3DPOOL_DEFAULT as librashader requires.
    const UINT nw = UINT(grid.native_w), nh = UINT(grid.native_h), rw = UINT(grid.rect_w), rh = UINT(grid.rect_h);
    ComPtr<IDirect3DTexture9> frame, upload, native, out;
    ComPtr<IDirect3DSurface9> native_rt, out_rt, readback;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &frame, nullptr)) ||
        FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &upload, nullptr)) ||
        FAILED(dev->CreateTexture(nw, nh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &native,
                                  nullptr)) ||
        FAILED(dev->CreateTexture(rw, rh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr)) ||
        FAILED(native->GetSurfaceLevel(0, &native_rt)) || FAILED(out->GetSurfaceLevel(0, &out_rt)) ||
        FAILED(dev->CreateOffscreenPlainSurface(rw, rh, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr)))
    {
        error = "could not create the Direct3D 9 textures";
        return false;
    }

    // Upload the picture (RGBA) into the frame (X8R8G8B8 stores blue, green, red, x).
    D3DLOCKED_RECT lr = {};
    if (FAILED(upload->LockRect(0, &lr, nullptr, 0)))
    {
        error = "could not upload the picture";
        return false;
    }
    for (UINT y = 0; y < h; ++y)
    {
        const uint8_t *src = rgba.data() + size_t(y) * w * 4;
        uint8_t *dst = static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch;
        for (UINT x = 0; x < w; ++x)
        {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 255;
        }
    }
    upload->UnlockRect(0);
    if (FAILED(dev->UpdateTexture(upload.Get(), frame.Get())))
    {
        error = "could not upload the picture";
        return false;
    }

    // The capture preset (Shader Model 3 version), with the grid set (as
    // FrameRenderer::set_capture_grid), and the preset.
    const ChainDevice chain_dev = {GraphicsApi::d3d9, reinterpret_cast<uint64_t>(dev.Get())};
    ShaderChain capture, chain;
    const std::string capture_path = capture_preset_path(true, error);
    if (capture_path.empty() || !capture.create(chain_dev, capture_path, error) ||
        !chain.create(chain_dev, preset_path, error))
        return false;
    const std::pair<const char *, int> grid_values[] = {
        {"rra_rect_x", grid.rect_x}, {"rra_rect_y", grid.rect_y},     {"rra_rect_w", grid.rect_w},
        {"rra_rect_h", grid.rect_h}, {"rra_native_w", grid.native_w}, {"rra_native_h", grid.native_h}};
    for (const auto &[name, value] : grid_values)
        if (!capture.set_param(name, float(value), error))
            return false;
    for (const auto &[name, value] : sets)
        if (!chain.set_param(name, value, error))
            return false;

    // Each frame, as FrameRenderer::render: the capture preset draws the native picture from
    // the frame, then the preset draws from the native picture into `out`. On Direct3D 9
    // librashader takes the input texture and the output surface (ChainImage in chain.h).
    for (int i = 0; i < frames; ++i)
    {
        if (!capture.frame(0, ChainImage{reinterpret_cast<uint64_t>(frame.Get()), 0, 0, w, h},
                           ChainImage{0, reinterpret_cast<uint64_t>(native_rt.Get()), 0, nw, nh}, 0, 0, int(nw),
                           int(nh), uint64_t(i), error) ||
            !chain.frame(0, ChainImage{reinterpret_cast<uint64_t>(native.Get()), 0, 0, nw, nh},
                         ChainImage{0, reinterpret_cast<uint64_t>(out_rt.Get()), 0, rw, rh}, 0, 0, int(rw), int(rh),
                         uint64_t(i), error))
            return false;
    }

    // Read `out` back and write it into the picture at the grid's rectangle.
    if (FAILED(dev->GetRenderTargetData(out_rt.Get(), readback.Get())) ||
        FAILED(readback->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    {
        error = "could not read the result back";
        return false;
    }
    for (UINT y = 0; y < rh; ++y)
    {
        const uint8_t *src = static_cast<const uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch;
        uint8_t *dst = rgba.data() + (size_t(grid.rect_y) + y) * w * 4 + size_t(grid.rect_x) * 4;
        for (UINT x = 0; x < rw; ++x)
        {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 255;
        }
    }
    readback->UnlockRect();
    return true;
}
