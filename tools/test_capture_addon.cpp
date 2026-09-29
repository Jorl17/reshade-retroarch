// Test-only ReShade add-on: saves screenshots and switches presets on a schedule,
// with no keyboard input (so tests never need focus).
//
// Script, from the RRA_TEST_SCRIPT environment variable, ';' separated:
//   <frame>:shot=<path.png>     save the frame (after all effects) as PNG
//   <frame>:preset=<path.ini>   switch ReShade preset
//   <frame>:copy=<src>|<dst>    copy a file (simulates a user dropping one in)

#include <reshade.hpp>

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

void on_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *,
                       reshade::api::resource_view, reshade::api::resource_view)
{
    for (const Step &s : g_steps)
    {
        if (s.frame != g_frame || s.action != "shot")
            continue;
        uint32_t w = 0, h = 0;
        runtime->get_screenshot_width_and_height(&w, &h);
        std::vector<uint8_t> pixels(size_t(w) * h * 4);
        const bool ok = runtime->capture_screenshot(pixels.data()) && save_png(widen(s.arg).c_str(), pixels.data(), w, h, w * 4);
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
    char buf[8192] = "";
    size_t len = 0;
    if (getenv_s(&len, buf, sizeof(buf), "RRA_TEST_SCRIPT") == 0 && len > 0)
        parse(buf);
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_present);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE)
{
    reshade::unregister_addon(addon_module);
}
