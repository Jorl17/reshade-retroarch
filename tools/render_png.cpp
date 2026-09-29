// Command-line tool: applies a RetroArch shader preset (.slangp file) to a screenshot
// and saves the result as a PNG, using the same code the ReShade add-on uses (grid
// detection, capture of the native image, librashader), but with no game and no ReShade.
// Renders with Direct3D 11 (the add-on's Renderer) or, with --api d3d12, with Direct3D 12
// (render_d3d12.h).
// The end-to-end test (tests/e2e.py) uses its output as the expected result, and
// tests/compat_sweep.py uses it to try every preset in a shader folder.
//
// Words used below:
//   native image   the game's own low-resolution picture (e.g. 424x240), before the game
//                  stretched it to fill the screen.
//   pixel grid     where that picture sits in the frame and at what resolution.
//   back buffer    the texture a game draws its frame into before it is shown.
//
//   render_png <preset.slangp> <input.png> <output.png> [options]
//     --frames N          number of frames to render before saving (default 3); presets
//                         that use earlier frames need a few to settle
//     --native out.png    also save the recovered native image
//     --grid auto|frame|WxH   how to find the native image: auto detects it (default),
//                         frame uses the whole frame as it is, WxH treats the whole frame
//                         as a W x H picture stretched to fill it
//     --set name=value    change a preset parameter before rendering (repeatable)
//     --hashes 1          print a checksum of the back buffer after every frame, to see
//                         whether frames of an unchanging input differ from each other
//                         (Direct3D 11 only)
//     --api d3d11|d3d12   graphics API to render with (default d3d11); --native and
//                         --hashes are Direct3D 11 only
//     --dxc DIR           Direct3D 12: folder with dxcompiler.dll and dxil.dll (default:
//                         this executable's folder)
//
// Exit code: 0 success, 1 failure (message on stderr), 2 bad command line.
// librashader.dll must be next to the executable.

#include "chain.h"
#include "grid_detect.h"
#include "librashader_api.h"
#include "png_io.h"
#include "render_d3d12.h"
#include "renderer.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

// Converts a UTF-16 string (a Windows command-line argument) to UTF-8, the encoding
// the add-on's code and librashader take paths and names in.
static std::string narrow(const wchar_t *s)
{
    std::string out(size_t(WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), int(out.size()), nullptr, nullptr);
    out.pop_back();
    return out;
}

// Saves the GPU texture `tex` as the PNG file `path`. The texture must hold 8-bit RGBA
// pixels. Returns false on failure.
// The GPU texture cannot be read by the CPU directly, so it is first copied into a
// "staging" texture: a CPU-readable copy with the same size and format.
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

// Returns a checksum (64-bit FNV-1a) of the pixels of GPU texture `tex` (8-bit RGBA), read
// back like read_back() does, or 0 on failure. Equal pictures give equal checksums.
static uint64_t checksum(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D *staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging)))
        return 0;
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    uint64_t hash = 0;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
    {
        hash = 1469598103934665603ull;
        for (UINT y = 0; y < d.Height; ++y)
        {
            const uint8_t *row = static_cast<const uint8_t *>(m.pData) + size_t(y) * m.RowPitch;
            for (UINT i = 0; i < d.Width * 4; ++i)
                hash = (hash ^ row[i]) * 1099511628211ull;
        }
        ctx->Unmap(staging, 0);
    }
    staging->Release();
    return hash;
}

// Entry point (wide-character arguments, so paths with any characters work). See the top
// of the file for the arguments. Loads the input picture into a stand-in back buffer,
// finds the pixel grid, loads the preset, renders `--frames` frames the way the add-on
// does each frame, and saves the back buffer (and optionally the native image).
int wmain(int argc, wchar_t **argv)
{
    if (argc < 4)
    {
        fwprintf(stderr, L"usage: render_png <preset.slangp> <input.png> <output.png> [--frames N] [--native out.png] "
                         L"[--grid auto|frame|WxH] [--set name=value]...\n");
        return 2;
    }
    // Options after the three fixed arguments, each followed by its value. Unknown
    // options are ignored; an option with no value ends the list.
    int frames = 3;
    bool hashes = false;
    std::wstring api = L"d3d11", dxc_dir;
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
        else if (opt == L"--hashes")
            hashes = _wtoi(argv[++a]) != 0;
        else if (opt == L"--api")
            api = argv[++a];
        else if (opt == L"--dxc")
            dxc_dir = argv[++a];
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

    // COM must be initialised before png_io.h can load or save images.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> rgba;
    UINT w = 0, h = 0;
    if (!load_png(argv[2], rgba, w, h))
    {
        fwprintf(stderr, L"could not load %s\n", argv[2]);
        return 1;
    }

    // Load librashader.dll from this executable's folder.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring exe_dir = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L"\\/") + 1);
    const std::wstring dll = exe_dir + L"librashader.dll";
    std::string err;
    if (!libra::load(dll, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // Decide where the native image is. "auto" runs the add-on's grid detector on the
    // input and fails (exit 1) if it finds none; "frame" and "WxH" cover the whole frame.
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

    // Direct3D 12: the whole render is in render_d3d12(); save its result and stop.
    if (api == L"d3d12")
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (!render_d3d12(rgba, w, h, grid, narrow(argv[1]), sets, frames, dxc_dir.empty() ? exe_dir : dxc_dir, err))
        {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        printf("%d frames in %.0f ms (Direct3D 12)\n", frames,
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (!save_png(argv[3], rgba.data(), w, h, w * 4))
        {
            fprintf(stderr, "could not save output\n");
            return 1;
        }
        printf("ok\n");
        return 0;
    }
    if (api != L"d3d11")
    {
        fwprintf(stderr, L"bad --api %s\n", api.c_str());
        return 2;
    }

    // A Direct3D 11 device on the default GPU, like the one a D3D11 game creates.
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx)))
    {
        fprintf(stderr, "D3D11CreateDevice failed\n");
        return 1;
    }

    // Stand-in for the game's back buffer: a texture of the input's size, filled with the
    // input picture, in the format most games use (8-bit RGBA). The renderer reads the
    // frame from it and writes its result into it, as the add-on does with a real one.
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

    // Set up the renderer (the add-on's capture and compositing code) and compile the
    // preset with librashader, then apply the --set parameter changes.
    Renderer renderer;
    ShaderChain chain;
    const auto t0 = std::chrono::steady_clock::now();
    if (!renderer.init(dev, err) || !chain.create(ChainDevice{GraphicsApi::d3d11, reinterpret_cast<uint64_t>(dev)}, narrow(argv[1]), err))
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

    // Each frame: put the input picture back into the back buffer (as the game would
    // draw it), copy it (snapshot), then recover the native image, run the preset on it
    // and write the result into the back buffer's grid rectangle.
    for (int i = 0; i < frames; ++i)
    {
        ctx->UpdateSubresource(backbuffer, 0, nullptr, rgba.data(), w * 4, 0); // the game draws a frame
        if (renderer.snapshot(ctx, backbuffer, err) == nullptr ||
            !renderer.render(ctx, grid, chain, backbuffer, uint64_t(i), err))
        {
            fprintf(stderr, "frame %d: %s\n", i, err.c_str());
            return 1;
        }
        if (hashes)
            printf("frame %d: %016llx\n", i, static_cast<unsigned long long>(checksum(dev, ctx, backbuffer)));
    }
    ctx->Flush();
    printf("%d frames in %.1f ms\n", frames,
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());

    // Save the last frame, and the native image it was made from if --native was given.
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
