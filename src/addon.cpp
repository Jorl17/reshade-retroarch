// ReShade add-on: runs RetroArch shader presets (.slangp, via librashader) on the
// game's native pixels.
//
// Which preset runs is decided by the selected ReShade preset: "<name>.slangp"
// next to "<name>.ini" (see companion.h). Switch ReShade presets and the
// RetroArch shader switches with them; End (effects off) turns it off too.

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "companion.h"
#include "detector.h"
#include "discovery.h"
#include "librashader_api.h"
#include "renderer.h"

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>

extern "C" __declspec(dllexport) const char *NAME = "RetroArch Shaders";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Runs RetroArch shader presets (.slangp) on the game's native pixels, through librashader.";

namespace fs = std::filesystem;
using namespace reshade::api;

namespace
{
constexpr const char *kSection = "RetroArchShaders";

enum NativeMode : int
{
    native_auto = 0,   // detect the game's pixel grid
    native_manual = 1, // fixed resolution, whole frame
    native_frame = 2,  // no native recovery: the preset gets the full frame
};

struct Settings
{
    int mode = native_auto;
    int manual_w = 320, manual_h = 240;
    std::string extra_paths; // ';' separated
};

// Per D3D11 device. The compiled chain lives here so that swap chain resets
// (resizes, alt-tab) do not recompile it.
struct DeviceData
{
    Renderer renderer;
    bool renderer_ready = false;
    ShaderChain chain;
    fs::path chain_source;
    fs::file_time_type chain_mtime{};
    std::string chain_error;
};

// Per effect runtime (swap chain). Kept across resets, see on_destroy_effect_runtime.
struct RuntimeData
{
    device *dev = nullptr;
    fs::path reshade_preset, companion;
    bool companion_exists = false;
    fs::file_time_type companion_mtime{};
    GridDetector detector;
    uint64_t frame = 0;
    double next_file_check = 0, last_effects = 0, created = 0;
    bool force_reload = false; // recompile even if unchanged (Revert, Use buttons)
    bool retry = false;        // companion reappeared: recompile if the last attempt failed
    bool warned_paused = false;
    std::string status, error;
};

HMODULE g_module = nullptr;
fs::path g_addon_dir, g_exe_dir;
Settings g_settings;
bool g_settings_loaded = false;

std::mutex g_mutex;
std::unordered_map<device *, std::unique_ptr<DeviceData>> g_devices;
std::unordered_map<effect_runtime *, std::unique_ptr<RuntimeData>> g_runtimes;

// Shader discovery, run in the background when the overlay is first opened.
std::vector<ShaderRoot> g_roots;
std::vector<PresetEntry> g_presets;
std::future<std::pair<std::vector<ShaderRoot>, std::vector<PresetEntry>>> g_scan;
bool g_scan_started = false;

// Overlay state.
char g_filter[128] = "";
int g_selected = -1;

double now_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void log_info(const std::string &s) { reshade::log::message(reshade::log::level::info, s.c_str()); }
void log_error(const std::string &s) { reshade::log::message(reshade::log::level::error, s.c_str()); }

void load_settings(effect_runtime *runtime)
{
    if (g_settings_loaded)
        return;
    g_settings_loaded = true;
    reshade::get_config_value(runtime, kSection, "NativeMode", g_settings.mode);
    reshade::get_config_value(runtime, kSection, "NativeWidth", g_settings.manual_w);
    reshade::get_config_value(runtime, kSection, "NativeHeight", g_settings.manual_h);
    char buf[2048] = "";
    size_t size = sizeof(buf);
    if (reshade::get_config_value(runtime, kSection, "ShaderPaths", buf, &size))
        g_settings.extra_paths = buf;
    if (g_settings.mode < native_auto || g_settings.mode > native_frame)
        g_settings.mode = native_auto;
}

void save_settings(effect_runtime *runtime)
{
    reshade::set_config_value(runtime, kSection, "NativeMode", g_settings.mode);
    reshade::set_config_value(runtime, kSection, "NativeWidth", g_settings.manual_w);
    reshade::set_config_value(runtime, kSection, "NativeHeight", g_settings.manual_h);
    reshade::set_config_value(runtime, kSection, "ShaderPaths", g_settings.extra_paths.c_str());
}

std::vector<fs::path> extra_paths()
{
    std::vector<fs::path> out;
    size_t start = 0;
    const std::string &s = g_settings.extra_paths;
    while (start <= s.size())
    {
        const size_t end = s.find(';', start);
        const std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty())
            out.push_back(fs::u8path(part));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

void start_scan()
{
    g_scan_started = true;
    const fs::path addon_dir = g_addon_dir, exe_dir = g_exe_dir;
    const std::vector<fs::path> extra = extra_paths();
    g_scan = std::async(std::launch::async, [addon_dir, exe_dir, extra] {
        std::vector<ShaderRoot> roots = find_shader_roots(addon_dir, exe_dir, extra);
        std::vector<PresetEntry> presets = scan_presets(roots);
        return std::make_pair(std::move(roots), std::move(presets));
    });
}

// Re-reads which companion applies and whether it changed on disk.
void refresh_companion(RuntimeData &rd)
{
    std::error_code ec;
    const bool exists = !rd.companion.empty() && fs::is_regular_file(rd.companion, ec);
    const fs::file_time_type mtime = exists ? fs::last_write_time(rd.companion, ec) : fs::file_time_type{};
    if (exists != rd.companion_exists || mtime != rd.companion_mtime)
    {
        rd.companion_exists = exists;
        rd.companion_mtime = mtime;
        rd.retry = true;
        rd.error.clear();
    }
}

void set_preset_path(RuntimeData &rd, const char *path)
{
    const fs::path preset = fs::u8path(path != nullptr ? path : "");
    if (preset == rd.reshade_preset)
    {
        refresh_companion(rd); // same preset (e.g. runtime reset on resize): keep what is loaded
        return;
    }
    rd.reshade_preset = preset;
    rd.companion = preset.empty() ? fs::path() : companion_path(preset);
    rd.companion_exists = false;
    rd.companion_mtime = {};
    refresh_companion(rd);
}

// ReShade reports preset switches made with its keys and overlay through an event,
// but not ones made through its API (by other add-ons). Checking every frame is
// cheap and catches both.
void poll_preset_path(effect_runtime *runtime, RuntimeData &rd)
{
    char path[4096] = "";
    size_t size = sizeof(path);
    runtime->get_current_preset_path(path, &size);
    if (rd.reshade_preset.u8string() != path)
        set_preset_path(rd, path);
}

RuntimeData *runtime_data(effect_runtime *runtime)
{
    const auto it = g_runtimes.find(runtime);
    return it == g_runtimes.end() ? nullptr : it->second.get();
}

void on_init_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    load_settings(runtime);
    std::unique_ptr<RuntimeData> &rd = g_runtimes[runtime];
    if (rd == nullptr)
    {
        rd = std::make_unique<RuntimeData>();
        rd->created = now_seconds();
    }
    rd->dev = runtime->get_device();
    char path[4096] = "";
    size_t size = sizeof(path);
    runtime->get_current_preset_path(path, &size);
    set_preset_path(*rd, path);
}

// Called both when a swap chain is resized and when it is destroyed. Only the
// size-dependent GPU resources are released; the grid and the compiled chain are
// kept for when the runtime comes back.
void on_destroy_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    RuntimeData *rd = runtime_data(runtime);
    if (rd == nullptr)
        return;
    command_list *cmd = runtime->get_command_queue()->get_immediate_command_list();
    rd->detector.shutdown(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));
}

void on_destroy_device(device *dev)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_runtimes.begin(); it != g_runtimes.end();)
        it = (it->second->dev == dev) ? g_runtimes.erase(it) : std::next(it);
    g_devices.erase(dev);
}

void on_set_current_preset_path(effect_runtime *runtime, const char *path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (RuntimeData *rd = runtime_data(runtime))
        set_preset_path(*rd, path);
}

// Loads librashader, the capture shader and the preset, as needed. Returns false
// (with rd.error set) when there is nothing to render with.
bool prepare(RuntimeData &rd, device *dev, ID3D11Device *d3d)
{
    std::string err;
    if (!libra::loaded())
    {
        static double next_attempt = 0;
        const double t = now_seconds();
        if (t < next_attempt)
            return false;
        if (!libra::load(g_addon_dir / L"librashader.dll", err))
        {
            next_attempt = t + 2.0; // the user may be copying it in right now
            if (rd.error != err)
                log_error("RetroArch Shaders: " + err);
            rd.error = err;
            return false;
        }
    }
    std::unique_ptr<DeviceData> &dd = g_devices[dev];
    if (dd == nullptr)
        dd = std::make_unique<DeviceData>();
    if (!dd->renderer_ready)
    {
        if (!dd->renderer.init(d3d, err))
        {
            rd.error = err;
            return false;
        }
        dd->renderer_ready = true;
    }
    // Switching back to a preset whose chain is still loaded costs nothing.
    if (rd.force_reload || dd->chain_source != rd.companion || dd->chain_mtime != rd.companion_mtime ||
        (rd.retry && !dd->chain.ready()))
    {
        rd.force_reload = false;
        rd.retry = false;
        dd->chain_source = rd.companion;
        dd->chain_mtime = rd.companion_mtime;
        dd->chain_error.clear();
        const auto t0 = std::chrono::steady_clock::now();
        if (dd->chain.create(d3d, rd.companion.u8string(), err))
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            log_info("Loaded " + rd.companion.u8string() + " (" + std::to_string(int(ms)) + " ms)");
        }
        else
        {
            dd->chain_error = err;
            log_error("Could not load " + rd.companion.u8string() + ": " + err);
        }
    }
    if (!dd->chain.ready())
    {
        rd.error = dd->chain_error;
        return false;
    }
    return true;
}

void on_begin_effects(effect_runtime *runtime, command_list *cmd_list, resource_view rtv, resource_view)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    RuntimeData *rd = runtime_data(runtime);
    if (rd == nullptr)
        return;
    const double t = now_seconds();
    rd->last_effects = t;
    poll_preset_path(runtime, *rd);
    if (t >= rd->next_file_check)
    {
        rd->next_file_check = t + 1.0;
        refresh_companion(*rd); // picks up files dropped in or edited while the game runs
    }
    if (!rd->companion_exists)
    {
        rd->status = "Off: no RetroArch shader for this ReShade preset.";
        return;
    }

    device *dev = runtime->get_device();
    if (dev->get_api() != device_api::d3d11)
    {
        rd->error = "Only Direct3D 11 games are supported for now.";
        return;
    }
    auto *d3d = reinterpret_cast<ID3D11Device *>(dev->get_native());
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
    if (!prepare(*rd, dev, d3d))
        return;
    DeviceData &dd = *g_devices[dev];

    auto *res = reinterpret_cast<ID3D11Resource *>(dev->get_resource_from_view(rtv).handle);
    ID3D11Texture2D *target = nullptr;
    if (res == nullptr || FAILED(res->QueryInterface(IID_PPV_ARGS(&target))))
        return;
    D3D11_TEXTURE2D_DESC desc;
    target->GetDesc(&desc);

    // Released on every path out of here.
    struct Holder
    {
        ID3D11Texture2D *p;
        ~Holder() { p->Release(); }
    } hold{target};

    std::string err;
    ID3D11Texture2D *snapshot = dd.renderer.snapshot(ctx, target, err);
    if (snapshot == nullptr)
    {
        rd->error = err;
        return;
    }

    PixelGrid grid;
    if (g_settings.mode == native_auto)
    {
        rd->detector.tick(d3d, ctx, snapshot, t);
        grid = rd->detector.grid();
        if (!grid.valid)
        {
            rd->error.clear();
            rd->status = "Waiting: " + rd->detector.status();
            return; // show the game untouched until its pixels are found
        }
    }
    else
    {
        grid.valid = true;
        grid.rect_w = int(desc.Width);
        grid.rect_h = int(desc.Height);
        grid.native_w = g_settings.mode == native_manual ? std::max(1, g_settings.manual_w) : int(desc.Width);
        grid.native_h = g_settings.mode == native_manual ? std::max(1, g_settings.manual_h) : int(desc.Height);
        grid.match = 1.0f;
    }

    if (!dd.renderer.render(ctx, grid, dd.chain, target, rd->frame++, err))
    {
        rd->error = err;
        return;
    }
    rd->error.clear();
    rd->status = "Running on " + grid.describe() + (rd->detector.provisional() ? " (re-checking)" : "");
}

// ReShade only calls reshade_begin_effects when it has effects to render. Explain
// when that is why nothing happens.
void on_reshade_present(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    RuntimeData *rd = runtime_data(runtime);
    if (rd == nullptr || !rd->companion_exists)
        return;
    const double t = now_seconds();
    if (t - rd->last_effects > 2.0 && t - rd->created > 5.0)
    {
        rd->status = "Paused: ReShade is not rendering effects. They may be switched off (End key), or no effect "
                     "files are installed; keep reshade-shaders\\Shaders\\RetroArchShaders.fx.";
        if (!rd->warned_paused)
        {
            rd->warned_paused = true;
            log_error("RetroArch Shaders: " + rd->status);
        }
    }
    else
        rd->warned_paused = false;
}

// ---------------------------------------------------------------------------
// Overlay

std::string companion_description(const RuntimeData &rd)
{
    fs::path target;
    if (read_reference(rd.companion, target))
        return target.filename().u8string() + "  (" + target.parent_path().u8string() + ")";
    return rd.companion.filename().u8string();
}

void draw_shader_picker(effect_runtime *runtime, RuntimeData &rd)
{
    if (!g_scan_started)
        start_scan();
    if (g_scan.valid() && g_scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        auto result = g_scan.get();
        g_roots = std::move(result.first);
        g_presets = std::move(result.second);
        g_selected = -1;
    }
    const bool scanning = g_scan.valid();

    if (scanning)
        ImGui::TextDisabled("Looking for shader presets...");
    else if (g_roots.empty())
    {
        ImGui::TextWrapped("No RetroArch shaders found. Put shader presets (for example the libretro "
                           "\"slang-shaders\" collection) in:");
        ImGui::TextUnformatted((g_addon_dir / L"retroarch-shaders").u8string().c_str());
    }
    else
        ImGui::Text("%d presets in %d folder(s).", int(g_presets.size()), int(g_roots.size()));

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Filter, e.g. crt-royale", g_filter, sizeof(g_filter));

    if (ImGui::BeginListBox("##presets", ImVec2(-1.0f, 260.0f)))
    {
        std::string filter = g_filter;
        for (char &c : filter)
            c = char(tolower(static_cast<unsigned char>(c)));
        for (int i = 0; i < int(g_presets.size()); ++i)
        {
            std::string label = g_presets[size_t(i)].label, lower = label;
            for (char &c : lower)
                c = char(tolower(static_cast<unsigned char>(c)));
            if (!filter.empty() && lower.find(filter) == std::string::npos)
                continue;
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), g_selected == i))
                g_selected = i;
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }

    const bool has_selection = g_selected >= 0 && g_selected < int(g_presets.size());
    ImGui::BeginDisabled(!has_selection || rd.companion.empty());
    if (ImGui::Button("Use for this ReShade preset"))
    {
        std::string err;
        if (write_companion(rd.companion, g_presets[size_t(g_selected)].path, {}, err))
        {
            refresh_companion(rd);
            rd.force_reload = true;
        }
        else
            rd.error = err;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!rd.companion_exists);
    if (ImGui::Button("Remove from this ReShade preset"))
    {
        // Renamed, not deleted: it may have been written by hand.
        std::error_code ec;
        fs::rename(rd.companion, rd.companion.wstring() + L".removed", ec);
        refresh_companion(rd);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(scanning);
    if (ImGui::Button("Rescan"))
        start_scan();
    ImGui::EndDisabled();
    (void)runtime;
}

void draw_resolution(effect_runtime *runtime, RuntimeData &rd)
{
    bool changed = false;
    changed |= ImGui::RadioButton("Detect automatically", &g_settings.mode, native_auto);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Fixed", &g_settings.mode, native_manual);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Whole frame", &g_settings.mode, native_frame);
    if (g_settings.mode == native_manual)
    {
        ImGui::SetNextItemWidth(120.0f);
        changed |= ImGui::InputInt("Width", &g_settings.manual_w);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        changed |= ImGui::InputInt("Height", &g_settings.manual_h);
        g_settings.manual_w = std::clamp(g_settings.manual_w, 16, 7680);
        g_settings.manual_h = std::clamp(g_settings.manual_h, 16, 4320);
    }
    if (g_settings.mode == native_auto)
    {
        ImGui::TextWrapped("%s", rd.detector.status().c_str());
        if (ImGui::Button("Detect again"))
            rd.detector.redetect();
    }
    if (changed)
        save_settings(runtime);
}

void draw_parameters(RuntimeData &rd)
{
    DeviceData *dd = nullptr;
    if (const auto it = g_devices.find(rd.dev); it != g_devices.end())
        dd = it->second.get();
    if (dd == nullptr || !dd->chain.ready())
    {
        ImGui::TextDisabled("No shader loaded.");
        return;
    }
    std::vector<ShaderParam> params = dd->chain.params();
    if (params.empty())
        ImGui::TextDisabled("This preset has no parameters.");
    for (const ShaderParam &p : params)
    {
        float v = p.value;
        ImGui::PushID(p.name.c_str());
        if (p.maximum > p.minimum && ImGui::SliderFloat(p.description.c_str(), &v, p.minimum, p.maximum, "%.4f"))
        {
            if (p.step > 0.0f)
                v = p.minimum + std::round((v - p.minimum) / p.step) * p.step;
            std::string err;
            dd->chain.set_param(p.name, v, err);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s (default %g)", p.name.c_str(), p.initial);
        ImGui::PopID();
    }

    fs::path target;
    const bool is_reference = read_reference(rd.companion, target);
    ImGui::BeginDisabled(!is_reference);
    if (ImGui::Button("Save to this ReShade preset"))
    {
        // Keep only values that differ from what the referenced preset sets.
        std::vector<std::pair<std::string, float>> overrides;
        const libra_instance_t &api = libra::api();
        libra_shader_preset_t base = nullptr;
        const bool have_base = api.preset_create(target.u8string().c_str(), &base) == nullptr;
        for (const ShaderParam &p : dd->chain.params())
        {
            float base_value = p.initial;
            if (have_base)
                api.preset_get_param(&base, p.name.c_str(), &base_value);
            if (std::fabs(p.value - base_value) > 1e-6f)
                overrides.emplace_back(p.name, p.value);
        }
        if (have_base)
            api.preset_free(&base);
        std::string err;
        if (!write_companion(rd.companion, target, overrides, err))
            rd.error = err;
        refresh_companion(rd);
    }
    ImGui::EndDisabled();
    if (!is_reference && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("This .slangp is a full preset, not a reference; edit the file directly.");
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
        rd.force_reload = true;
}

void draw_overlay(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    RuntimeData *rd = runtime_data(runtime);
    if (rd == nullptr)
    {
        ImGui::TextDisabled("Not initialised yet.");
        return;
    }

    ImGui::Text("ReShade preset: %s", rd->reshade_preset.filename().u8string().c_str());
    if (rd->companion_exists)
        ImGui::TextWrapped("RetroArch shader: %s", companion_description(*rd).c_str());
    else
        ImGui::TextDisabled("No RetroArch shader for this ReShade preset.");
    if (!rd->error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", rd->error.c_str());
    else if (!rd->status.empty())
        ImGui::TextWrapped("%s", rd->status.c_str());
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Shader", ImGuiTreeNodeFlags_DefaultOpen))
        draw_shader_picker(runtime, *rd);
    if (ImGui::CollapsingHeader("Game resolution"))
        draw_resolution(runtime, *rd);
    if (ImGui::CollapsingHeader("Shader parameters"))
        draw_parameters(*rd);
}
} // namespace

// ReShade calls AddonInit/AddonUninit outside DllMain (not under the loader lock),
// which matters here: tearing down joins the grid detectors' worker threads.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE)
{
    if (!reshade::register_addon(addon_module))
        return false;
    g_module = addon_module;
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(addon_module, buf, MAX_PATH);
    g_addon_dir = fs::path(buf).parent_path();
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_exe_dir = fs::path(buf).parent_path();

    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(on_set_current_preset_path);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::register_overlay("RetroArch Shaders", draw_overlay);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_runtimes.clear(); // joins detector threads
        g_devices.clear();
    }
    if (g_scan.valid())
        g_scan.wait();
    reshade::unregister_overlay("RetroArch Shaders", draw_overlay);
    reshade::unregister_addon(addon_module);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
