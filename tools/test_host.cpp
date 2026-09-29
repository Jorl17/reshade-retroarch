// Stand-in for a game, to test ReShade and add-ons without launching one.
//
// Opens a small window WITHOUT taking focus, creates a swap chain and presents
// frames, drawing PNGs into the back buffer. Resolution changes are scripted:
//
//   test_host --frames N [--size WxH] [--image F:path.png]... [--resize F:WxH]...
//
// --image F:path   from frame F, draw this image (it must match the back buffer
//                  size; mismatched frames are cleared to black)
// --resize F:WxH   at frame F, resize the swap chain buffers (like a game switching
//                  resolution or toggling fullscreen)
//
// Put ReShade's d3d11.dll (and any add-ons) next to this executable.

#include "png_io.h"

#include <d3d11.h>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
struct Image
{
    UINT w = 0, h = 0;
    ID3D11Texture2D *tex = nullptr;
};

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE)
    {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

int wmain(int argc, wchar_t **argv)
{
    int frames = 120;
    UINT bw = 3840, bh = 2160;
    std::map<int, std::wstring> image_at;
    std::map<int, std::pair<UINT, UINT>> resize_at;
    for (int a = 1; a + 1 < argc; ++a)
    {
        const std::wstring opt = argv[a], val = argv[a + 1];
        ++a;
        const size_t colon = val.find(L':');
        if (opt == L"--frames")
            frames = _wtoi(val.c_str());
        else if (opt == L"--size")
            swscanf_s(val.c_str(), L"%ux%u", &bw, &bh);
        else if (opt == L"--image" && colon != std::wstring::npos)
            image_at[_wtoi(val.c_str())] = val.substr(colon + 1);
        else if (opt == L"--resize" && colon != std::wstring::npos)
        {
            UINT w = 0, h = 0;
            swscanf_s(val.c_str() + colon + 1, L"%ux%u", &w, &h);
            resize_at[_wtoi(val.c_str())] = {w, h};
        }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RRATestHost";
    RegisterClassW(&wc);
    // Small, in a corner, never activated: it must not steal focus from the user.
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"reshade-retroarch test host",
                                WS_POPUP | WS_BORDER, 0, 0, 480, 270, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = bw;
    sd.BufferDesc.Height = bh;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain *swap = nullptr;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                     D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx);
    if (FAILED(hr))
    {
        fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed 0x%08lX\n", hr);
        return 1;
    }

    std::map<std::wstring, Image> images;
    auto load = [&](const std::wstring &path) -> Image * {
        auto it = images.find(path);
        if (it != images.end())
            return &it->second;
        std::vector<uint8_t> rgba;
        Image img;
        if (!load_png(path.c_str(), rgba, img.w, img.h))
        {
            fwprintf(stderr, L"could not load %s\n", path.c_str());
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = img.w;
        td.Height = img.h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA init = {rgba.data(), img.w * 4, 0};
        if (FAILED(dev->CreateTexture2D(&td, &init, &img.tex)))
            return nullptr;
        return &(images[path] = img);
    };

    Image *current = nullptr;
    for (int i = 0; i < frames; ++i)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (auto it = resize_at.find(i); it != resize_at.end())
        {
            bw = it->second.first;
            bh = it->second.second;
            ctx->ClearState();
            ctx->Flush();
            const HRESULT rh = swap->ResizeBuffers(0, bw, bh, DXGI_FORMAT_UNKNOWN, 0);
            printf("frame %d: resized to %ux%u (0x%08lX)\n", i, bw, bh, rh);
        }
        if (auto it = image_at.find(i); it != image_at.end())
            current = load(it->second);

        ID3D11Texture2D *bb = nullptr;
        swap->GetBuffer(0, IID_PPV_ARGS(&bb));
        if (current != nullptr && current->w == bw && current->h == bh)
            ctx->CopyResource(bb, current->tex);
        else
        {
            ID3D11RenderTargetView *rtv = nullptr;
            dev->CreateRenderTargetView(bb, nullptr, &rtv);
            const float black[4] = {0, 0, 0, 1};
            ctx->ClearRenderTargetView(rtv, black);
            rtv->Release();
        }
        bb->Release();
        swap->Present(0, 0);
        Sleep(8);
    }
    printf("presented %d frames\n", frames);

    for (auto &[path, img] : images)
        img.tex->Release();
    ctx->ClearState();
    ctx->Release();
    dev->Release();
    swap->Release();
    DestroyWindow(hwnd);
    return 0;
}
