// Test-only ReShade add-on: saves screenshots and switches presets on a schedule,
// with no keyboard input (so tests never need focus).
//
// Script, from the RRA_TEST_SCRIPT environment variable, ';' separated:
//   <frame>:shot=<path.png>     save the frame (after all effects) as PNG
//   <frame>:preset=<path.ini>   switch ReShade preset
//   <frame>:copy=<src>|<dst>    copy a file (simulates a user dropping one in)

#include <reshade.hpp>

#include <d3d11.h>

#include "png_io.h"

#include <cstdlib>
#include <string>
#include <vector>

extern "C" __declspec(dllexport) const char *NAME = "reshade-retroarch test capture";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Scripted screenshots for tests.";

namespace
{
struct Step
{
    unsigned frame;
    std::string action, arg;
};
std::vector<Step> g_steps;
unsigned g_frame = 0;

std::wstring widen(const std::string &s)
{
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    w.pop_back();
    return w;
}

void parse(const std::string &script)
{
    size_t start = 0;
    while (start < script.size())
    {
        size_t end = script.find(';', start);
        if (end == std::string::npos)
            end = script.size();
        const std::string item = script.substr(start, end - start);
        const size_t colon = item.find(':'), eq = item.find('=');
        if (colon != std::string::npos && eq != std::string::npos && eq > colon)
            g_steps.push_back({unsigned(std::stoul(item.substr(0, colon))), item.substr(colon + 1, eq - colon - 1),
                               item.substr(eq + 1)});
        start = end + 1;
    }
}

// Reads the frame back as 8-bit RGBA, whatever the back buffer format. (ReShade's
// capture_screenshot returns 10-bit frames still packed, and fails for sRGB ones.)
bool read_frame(reshade::api::effect_runtime *runtime, reshade::api::resource_view rtv, std::vector<uint8_t> &out,
                uint32_t &w, uint32_t &h)
{
    auto *res = reinterpret_cast<ID3D11Resource *>(runtime->get_device()->get_resource_from_view(rtv).handle);
    auto *dev = reinterpret_cast<ID3D11Device *>(runtime->get_device()->get_native());
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(
        runtime->get_command_queue()->get_immediate_command_list()->get_native());
    ID3D11Texture2D *tex = nullptr;
    if (res == nullptr || FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))))
        return false;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    const DXGI_FORMAT format = d.Format;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D *staging = nullptr;
    bool ok = d.SampleDesc.Count == 1 && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging));
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (ok)
    {
        ctx->CopyResource(staging, tex);
        ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    }
    if (ok)
    {
        w = d.Width;
        h = d.Height;
        out.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *src = static_cast<const uint8_t *>(m.pData) + size_t(y) * m.RowPitch;
            uint8_t *dst = out.data() + size_t(y) * w * 4;
            for (uint32_t x = 0; x < w; ++x, src += 4, dst += 4)
            {
                switch (format)
                {
                case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                {
                    uint32_t v;
                    memcpy(&v, src, 4);
                    dst[0] = uint8_t((v & 0x3FF) >> 2);
                    dst[1] = uint8_t(((v >> 10) & 0x3FF) >> 2);
                    dst[2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
                    dst[3] = 255;
                    break;
                }
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8X8_TYPELESS:
                case DXGI_FORMAT_B8G8R8X8_UNORM:
                case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = 255;
                    break;
                default: // R8G8B8A8 family (the stored bytes, sRGB or not)
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                    break;
                }
            }
        }
        ctx->Unmap(staging, 0);
    }
    if (staging != nullptr)
        staging->Release();
    tex->Release();
    return ok;
}

void on_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *,
                       reshade::api::resource_view rtv, reshade::api::resource_view)
{
    for (const Step &s : g_steps)
    {
        if (s.frame != g_frame || s.action != "shot")
            continue;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> pixels;
        const bool ok = read_frame(runtime, rtv, pixels, w, h) && save_png(widen(s.arg).c_str(), pixels.data(), w, h, w * 4);
        reshade::log::message(ok ? reshade::log::level::info : reshade::log::level::error,
                              ("test capture: frame " + std::to_string(g_frame) + " -> " + s.arg).c_str());
    }
}

void on_present(reshade::api::effect_runtime *runtime)
{
    ++g_frame;
    for (const Step &s : g_steps)
    {
        if (s.frame != g_frame)
            continue;
        if (s.action == "preset")
        {
            runtime->set_current_preset_path(s.arg.c_str());
            reshade::log::message(reshade::log::level::info,
                                  ("test capture: frame " + std::to_string(g_frame) + " preset " + s.arg).c_str());
        }
        else if (s.action == "copy")
        {
            const size_t bar = s.arg.find('|');
            const bool ok = bar != std::string::npos &&
                            CopyFileW(widen(s.arg.substr(0, bar)).c_str(), widen(s.arg.substr(bar + 1)).c_str(), FALSE);
            reshade::log::message(ok ? reshade::log::level::info : reshade::log::level::error,
                                  ("test capture: frame " + std::to_string(g_frame) + " copy " + s.arg).c_str());
        }
    }
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE)
{
    if (!reshade::register_addon(addon_module))
        return false;
    size_t len = 0;
    getenv_s(&len, nullptr, 0, "RRA_TEST_SCRIPT"); // size, including the terminator
    if (len > 0)
    {
        std::string buf(len, '\0');
        if (getenv_s(&len, buf.data(), buf.size(), "RRA_TEST_SCRIPT") == 0)
            parse(buf.c_str());
        reshade::log::message(reshade::log::level::info,
                              ("test capture: " + std::to_string(g_steps.size()) + " scripted steps").c_str());
    }
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_present);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE)
{
    reshade::unregister_addon(addon_module);
}
