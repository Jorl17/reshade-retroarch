// Runs a RetroArch preset on a screenshot, through the same code the add-on uses
// (grid detection, capture, librashader). No game, no ReShade.
//
//   render_png <preset.slangp> <input.png> <output.png> [options]
//     --frames N          frames to render before saving (default 3)
//     --native out.png    also save the recovered native image
//     --grid auto|frame|WxH   how to find the native image (default auto)
//     --set name=value    override a preset parameter (repeatable)
//
// librashader.dll must be next to the executable.

#include "chain.h"
#include "grid_detect.h"
#include "librashader_api.h"
#include "png_io.h"
#include "renderer.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

static std::string narrow(const wchar_t *s)
{
    std::string out(size_t(WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), int(out.size()), nullptr, nullptr);
    out.pop_back();
    return out;
}

static bool read_back(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const wchar_t *path)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D *staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging)))
        return false;
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    if (ok)
    {
        ok = save_png(path, static_cast<const uint8_t *>(m.pData), d.Width, d.Height, m.RowPitch);
        ctx->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 4)
    {
        fwprintf(stderr, L"usage: render_png <preset.slangp> <input.png> <output.png> [--frames N] [--native out.png] "
                         L"[--grid auto|frame|WxH] [--set name=value]...\n");
        return 2;
    }
    int frames = 3;
    const wchar_t *native_out = nullptr;
    std::wstring grid_mode = L"auto";
    std::vector<std::pair<std::string, float>> sets;
    for (int a = 4; a < argc; ++a)
    {
        const std::wstring opt = argv[a];
        if (a + 1 >= argc)
            break;
        if (opt == L"--frames")
            frames = _wtoi(argv[++a]);
        else if (opt == L"--native")
            native_out = argv[++a];
        else if (opt == L"--grid")
            grid_mode = argv[++a];
        else if (opt == L"--set")
        {
            const std::string kv = narrow(argv[++a]);
            const size_t eq = kv.find('=');
            if (eq != std::string::npos)
                sets.emplace_back(kv.substr(0, eq), float(atof(kv.c_str() + eq + 1)));
        }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> rgba;
    UINT w = 0, h = 0;
    if (!load_png(argv[2], rgba, w, h))
    {
        fwprintf(stderr, L"could not load %s\n", argv[2]);
        return 1;
    }

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dll = exe;
    dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"librashader.dll";
    std::string err;
    if (!libra::load(dll, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx)))
    {
        fprintf(stderr, "D3D11CreateDevice failed\n");
        return 1;
    }

    // Stand-in for the game's back buffer (same format as most games use).
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    const D3D11_SUBRESOURCE_DATA init = {rgba.data(), w * 4, 0};
    ID3D11Texture2D *backbuffer = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &init, &backbuffer)) ||
        FAILED(dev->CreateRenderTargetView(backbuffer, nullptr, &rtv)))
    {
        fprintf(stderr, "could not create the back buffer stand-in\n");
        return 1;
    }

    PixelGrid grid;
    if (grid_mode == L"auto")
    {
        FrameView f;
        f.data = rgba.data();
        f.width = int(w);
        f.height = int(h);
        f.pitch = size_t(w) * 4;
        std::string log;
        grid = detect_grid(f, &log);
        printf("grid: %s  [%s]\n", grid.describe().c_str(), log.c_str());
        if (!grid.valid)
            return 1;
    }
    else
    {
        grid.valid = true;
        grid.rect_w = int(w);
        grid.rect_h = int(h);
        grid.native_w = int(w);
        grid.native_h = int(h);
        if (grid_mode != L"frame" && swscanf_s(grid_mode.c_str(), L"%dx%d", &grid.native_w, &grid.native_h) != 2)
        {
            fwprintf(stderr, L"bad --grid %s\n", grid_mode.c_str());
            return 2;
        }
        printf("grid: %s\n", grid.describe().c_str());
    }

    Renderer renderer;
    ShaderChain chain;
    const auto t0 = std::chrono::steady_clock::now();
    if (!renderer.init(dev, err) || !chain.create(dev, narrow(argv[1]), err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const auto t1 = std::chrono::steady_clock::now();
    printf("preset loaded in %.0f ms, %d parameters\n", std::chrono::duration<double, std::milli>(t1 - t0).count(),
           int(chain.params().size()));
    for (const auto &[name, value] : sets)
    {
        if (!chain.set_param(name, value, err))
        {
            fprintf(stderr, "--set %s: %s\n", name.c_str(), err.c_str());
            return 1;
        }
        printf("set %s = %g\n", name.c_str(), value);
    }

    for (int i = 0; i < frames; ++i)
    {
        ctx->UpdateSubresource(backbuffer, 0, nullptr, rgba.data(), w * 4, 0); // the game draws a frame
        if (renderer.snapshot(ctx, backbuffer, err) == nullptr ||
            !renderer.render(ctx, grid, chain, backbuffer, uint64_t(i), err))
        {
            fprintf(stderr, "frame %d: %s\n", i, err.c_str());
            return 1;
        }
    }
    ctx->Flush();
    printf("%d frames in %.1f ms\n", frames,
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());

    if (!read_back(dev, ctx, backbuffer, argv[3]) ||
        (native_out != nullptr && !read_back(dev, ctx, renderer.native_texture(), native_out)))
    {
        fprintf(stderr, "could not save output\n");
        return 1;
    }
    chain.destroy();
    renderer.shutdown();
    rtv->Release();
    backbuffer->Release();
    ctx->Release();
    dev->Release();
    printf("ok\n");
    return 0;
}
