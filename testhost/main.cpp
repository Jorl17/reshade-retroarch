// Stand-in for a game, to test ReShade add-ons on every graphics API without one.
//
// Opens a small window WITHOUT taking focus and presents test pictures through the
// chosen API, unchanged, so every API shows byte-identical frames:
//
//   test_host --api d3d9|d3d10|d3d11|d3d12|opengl|vulkan   (default d3d11)
//             --frames N [--size WxH] [--format rgba8|rgba8srgb|rgb10a2] [--hdr10 1] [--warp 1]
//             [--image F:path.png]... [--resize F:WxH]... [--selfshot F:path.png]...
//
// --image F:path     from frame F, draw this picture (it must match the back buffer
//                    size; other frames are black)
// --resize F:WxH     at frame F, resize the back buffer (a game switching resolution)
// --selfshot F:path  at frame F, save the back buffer as drawn by the host itself,
//                    before ReShade: proves the host shows the picture unchanged
// --warp 1           software rendering (Direct3D only)
//
// Exit codes: 0 ok, 1 error, 3 the API cannot do what was asked (format, HDR10).
// Put ReShade (and add-ons) next to this executable to test them.

#include "../tools/png_io.h"
#include "backend.h"

#include <cstdio>
#include <map>

namespace
{
LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE)
    {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (msg == WM_GETMINMAXINFO) // allow windows larger than the screen (the OpenGL host needs them)
    {
        reinterpret_cast<MINMAXINFO *>(lp)->ptMaxTrackSize = {16384, 16384};
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::string narrow(const std::wstring &w)
{
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), int(s.size()), nullptr, nullptr);
    s.pop_back();
    return s;
}
}

int wmain(int argc, wchar_t **argv)
{
    std::wstring api = L"d3d11";
    int frames = 120;
    Options o;
    o.width = 3840;
    o.height = 2160;
    std::map<int, std::wstring> image_at, shot_at;
    std::map<int, std::pair<UINT, UINT>> resize_at;
    for (int a = 1; a + 1 < argc; a += 2)
    {
        const std::wstring opt = argv[a], val = argv[a + 1];
        const size_t colon = val.find(L':');
        if (opt == L"--api")
            api = val;
        else if (opt == L"--frames")
            frames = _wtoi(val.c_str());
        else if (opt == L"--size")
            swscanf_s(val.c_str(), L"%ux%u", &o.width, &o.height);
        else if (opt == L"--format")
            o.format = val == L"rgb10a2" ? Format::rgb10a2 : val == L"rgba8srgb" ? Format::rgba8srgb : Format::rgba8;
        else if (opt == L"--hdr10")
            o.hdr10 = val == L"1";
        else if (opt == L"--warp")
            o.warp = val == L"1";
        else if (opt == L"--image" && colon != std::wstring::npos)
            image_at[_wtoi(val.c_str())] = val.substr(colon + 1);
        else if (opt == L"--selfshot" && colon != std::wstring::npos)
            shot_at[_wtoi(val.c_str())] = val.substr(colon + 1);
        else if (opt == L"--resize" && colon != std::wstring::npos)
        {
            UINT w = 0, h = 0;
            swscanf_s(val.c_str() + colon + 1, L"%ux%u", &w, &h);
            resize_at[_wtoi(val.c_str())] = {w, h};
        }
    }

    std::unique_ptr<Backend> backend = api == L"d3d9"     ? make_d3d9()
                                       : api == L"d3d10"  ? make_d3d10()
                                       : api == L"d3d11"  ? make_d3d11()
                                       : api == L"d3d12"  ? make_d3d12()
                                       : api == L"opengl" ? make_opengl()
                                       : api == L"vulkan" ? make_vulkan()
                                                          : nullptr;
    if (backend == nullptr)
    {
        fwprintf(stderr, L"unknown --api %s\n", api.c_str());
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RRATestHost";
    wc.style = CS_OWNDC; // OpenGL needs a stable device context
    RegisterClassW(&wc);
    // Small, in a corner, never activated: it must not steal focus from the user.
    o.hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"reshade-retroarch test host",
                             WS_POPUP | WS_BORDER, 0, 0, 480, 270, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);

    std::string err;
    bool unsupported = false;
    if (!backend->init(o, err, unsupported))
    {
        fprintf(stderr, "%s: %s\n", narrow(api).c_str(), err.c_str());
        return unsupported ? 3 : 1;
    }

    std::map<std::wstring, Image> images;
    auto load = [&](const std::wstring &path) -> const Image * {
        if (auto it = images.find(path); it != images.end())
            return &it->second;
        Image img;
        if (!load_png(path.c_str(), img.rgba, img.w, img.h))
        {
            fwprintf(stderr, L"could not load %s\n", path.c_str());
            return nullptr;
        }
        return &(images[path] = std::move(img));
    };

    UINT bw = o.width, bh = o.height;
    const Image *current = nullptr;
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
            if (!backend->resize(bw, bh, err))
            {
                fprintf(stderr, "frame %d: resize failed: %s\n", i, err.c_str());
                return 1;
            }
            printf("frame %d: resized to %ux%u\n", i, bw, bh);
        }
        if (auto it = image_at.find(i); it != image_at.end())
            current = load(it->second);

        const Image *img = current != nullptr && current->w == bw && current->h == bh ? current : nullptr;
        if (!backend->draw(img, err))
        {
            fprintf(stderr, "frame %d: draw failed: %s\n", i, err.c_str());
            return 1;
        }
        if (auto it = shot_at.find(i); it != shot_at.end())
        {
            Image shot;
            if (!backend->read_back(shot, err) ||
                !save_png(it->second.c_str(), shot.rgba.data(), shot.w, shot.h, shot.w * 4))
            {
                fprintf(stderr, "frame %d: self shot failed: %s\n", i, err.c_str());
                return 1;
            }
        }
        if (!backend->present(err))
        {
            fprintf(stderr, "frame %d: present failed: %s\n", i, err.c_str());
            return 1;
        }
        Sleep(8);
    }
    printf("presented %d frames\n", frames);
    backend.reset();
    DestroyWindow(o.hwnd);
    return 0;
}
