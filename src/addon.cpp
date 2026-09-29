// ReShade add-on: runs RetroArch shader presets (.slangp, via librashader) on the
// game's native pixels.
//
// Which preset runs is decided by the selected ReShade preset: "<name>.slangp"
// next to "<name>.ini" (see companion.h). Switch ReShade presets and the
// RetroArch shader switches with them; turning effects off turns it off too.

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "companion.h"
#include "detector.h"
#include "discovery.h"
#include "librashader_api.h"
#include "renderer.h"
#include "utf8.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
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

// Settings for the whole add-on, in the global ReShade.ini (section [RetroArchShaders]).
struct Settings
{
    int mode = native_auto;
    int manual_w = 320, manual_h = 240;
    std::vector<fs::path> extra_paths; // ShaderPaths, ';' or ',' separated
};

// Per effect runtime (one per swap chain). Kept across swap chain resizes, so the
// compiled chain and the grid survive them; dropped when the swap chain or the
// device goes away.
struct RuntimeData
{
    device *dev = nullptr;
    uint64_t native = 0; // the swap chain's native handle
    fs::path reshade_preset, companion;
    bool companion_exists = false;
    fs::file_time_type companion_mtime{};

    Renderer renderer;
    bool renderer_ready = false;
    ShaderChain chain;
    fs::path chain_source;
    fs::file_time_type chain_mtime{};
    std::string chain_error;

    GridDetector detector;
    bool detect_pending = false; // a snapshot is waiting for the detector (see on_reshade_present)
    uint64_t frame = 0;
    double next_file_check = 0, last_effects = 0, created = 0;
    bool force_reload = false; // recompile even if unchanged (Revert, Use buttons)
    bool retry = false;        // companion reappeared: recompile if the last attempt failed
    bool warned_paused = false;
    std::string status, error;
    std::string logged_error; // the last error written to ReShade.log, so each one is logged once
    std::string action_error; // from the panel's buttons; stays until the next successful action
};

// Shader discovery runs on its own thread. The object is deliberately leaked if
// the game exits without unloading add-ons: destroying a running std::thread (or
// an unfinished std::async future) in a static destructor terminates or hangs
// the process.
struct Scan
{
    std::atomic<bool> cancel{false}, done{false};
    std::vector<ShaderRoot> roots;
    std::vector<PresetEntry> presets;
    int skipped = 0;
    std::string error;
    std::thread thread;
};

HMODULE g_module = nullptr;
fs::path g_addon_dir, g_exe_dir;
Settings g_settings;
bool g_settings_loaded = false;

std::mutex g_mutex;
std::unordered_map<effect_runtime *, std::unique_ptr<RuntimeData>> g_runtimes;
std::unordered_map<uint64_t, swapchain *> g_swapchains; // by native handle, to read the output colour space

Scan *g_scan = nullptr;
std::vector<ShaderRoot> g_roots;
std::vector<PresetEntry> g_presets;
int g_scan_skipped = 0;
std::string g_scan_error;
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

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The global config (nullptr runtime): passing a runtime would make ReShade reload
// that runtime's whole configuration on every write, which can switch presets.
void load_settings()
{
    if (g_settings_loaded)
        return;
    g_settings_loaded = true;
    reshade::get_config_value(nullptr, kSection, "NativeMode", g_settings.mode);
    reshade::get_config_value(nullptr, kSection, "NativeWidth", g_settings.manual_w);
    reshade::get_config_value(nullptr, kSection, "NativeHeight", g_settings.manual_h);
    if (g_settings.mode < native_auto || g_settings.mode > native_frame)
        g_settings.mode = native_auto;
    g_settings.manual_w = std::clamp(g_settings.manual_w, 16, 7680);
    g_settings.manual_h = std::clamp(g_settings.manual_h, 16, 4320);

    // ReShade splits values on ',' and returns the parts separated by '\0'.
    size_t size = 0;
    if (reshade::get_config_value(nullptr, kSection, "ShaderPaths", nullptr, &size) && size > 1)
    {
        std::string buf(size, '\0');
        if (reshade::get_config_value(nullptr, kSection, "ShaderPaths", buf.data(), &size))
        {
            buf.resize(std::min(size, buf.size()));
            std::string part;
            for (size_t i = 0; i <= buf.size(); ++i)
            {
                if (i == buf.size() || buf[i] == '\0' || buf[i] == ';' || buf[i] == ',')
                {
                    if (const std::string p = trim(part); !p.empty())
                        g_settings.extra_paths.push_back(path_from_utf8(p));
                    part.clear();
                }
                else
                    part += buf[i];
            }
        }
    }
}

void save_settings()
{
    reshade::set_config_value(nullptr, kSection, "NativeMode", g_settings.mode);
    reshade::set_config_value(nullptr, kSection, "NativeWidth", g_settings.manual_w);
    reshade::set_config_value(nullptr, kSection, "NativeHeight", g_settings.manual_h);
}

void start_scan()
{
    if (g_scan != nullptr)
        return; // one at a time
    g_scan_started = true;
    Scan *scan = new Scan();
    const fs::path addon_dir = g_addon_dir, exe_dir = g_exe_dir;
    const std::vector<fs::path> extra = g_settings.extra_paths;
    scan->thread = std::thread([scan, addon_dir, exe_dir, extra] {
        try
        {
            scan->roots = find_shader_roots(addon_dir, exe_dir, extra);
            scan->presets = scan_presets(scan->roots, scan->cancel, scan->skipped);
        }
        catch (const std::exception &e)
        {
            scan->error = e.what();
        }
        scan->done.store(true, std::memory_order_release);
    });
    g_scan = scan;
}

// Picks up a finished scan. Returns true while one is running.
bool poll_scan()
{
    if (g_scan == nullptr)
        return false;
    if (!g_scan->done.load(std::memory_order_acquire))
        return true;
    g_scan->thread.join();
    g_roots = std::move(g_scan->roots);
    g_presets = std::move(g_scan->presets);
    g_scan_skipped = g_scan->skipped;
    g_scan_error = g_scan->error;
    if (!g_scan_error.empty())
        log_error("RetroArch Shaders: shader scan failed: " + g_scan_error);
    delete g_scan;
    g_scan = nullptr;
    g_selected = -1;
    return false;
}

void stop_scan()
{
    if (g_scan == nullptr)
        return;
    g_scan->cancel = true;
    g_scan->thread.join();
    delete g_scan;
    g_scan = nullptr;
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
    const fs::path preset = path_from_utf8(path != nullptr ? path : "");
    if (preset == rd.reshade_preset)
    {
        refresh_companion(rd); // same preset (e.g. runtime reset on resize): keep what is loaded
        return;
    }
    rd.reshade_preset = preset;
    rd.companion = preset.empty() ? fs::path() : companion_path(preset);
    rd.companion_exists = false;
    rd.companion_mtime = {};
    rd.action_error.clear();
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
    if (utf8_from_path(rd.reshade_preset) != path)
        set_preset_path(rd, path);
}

RuntimeData *runtime_data(effect_runtime *runtime)
{
    const auto it = g_runtimes.find(runtime);
    return it == g_runtimes.end() ? nullptr : it->second.get();
}

// Shown in the overlay, and logged once (the overlay is not open most of the time).
void set_error(RuntimeData &rd, const std::string &error)
{
    rd.error = error;
    if (error != rd.logged_error)
        log_error("RetroArch Shaders: " + error);
    rd.logged_error = error;
}

void set_action_error(RuntimeData &rd, const std::string &error)
{
    rd.action_error = error;
    log_error("RetroArch Shaders: " + error);
}

void on_init_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    load_settings();
    std::unique_ptr<RuntimeData> &rd = g_runtimes[runtime];
    if (rd == nullptr)
        rd = std::make_unique<RuntimeData>();
    rd->dev = runtime->get_device();
    rd->native = runtime->get_native();
    // ReShade reloads all effects after every init (also after a resize); give it time
    // before concluding that effects are not being rendered.
    rd->created = now_seconds();
    rd->warned_paused = false;
    char path[4096] = "";
    size_t size = sizeof(path);
    runtime->get_current_preset_path(path, &size);
    set_preset_path(*rd, path);
}

// Called both when a swap chain is resized and when it is destroyed. Only the
// detector's pending readback is dropped here; the grid and the compiled chain are
// kept for when the runtime comes back (see on_destroy_swapchain for the rest).
void on_destroy_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    RuntimeData *rd = runtime_data(runtime);
    if (rd == nullptr)
        return;
    command_list *cmd = runtime->get_command_queue()->get_immediate_command_list();
    rd->detector.shutdown(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));
    rd->detect_pending = false;
}

void on_reloaded_effects(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (RuntimeData *rd = runtime_data(runtime))
        rd->created = now_seconds();
}

void on_init_swapchain(swapchain *sc, bool)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_swapchains[sc->get_native()] = sc;
}

// ReShade raises this after destroy_effect_runtime, both for resizes and for real
// destruction. On destruction the runtime object is deleted right after, so its
// data goes too (a later runtime may even get the same address).
void on_destroy_swapchain(swapchain *sc, bool resize)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t native = sc->get_native();
    g_swapchains.erase(native);
    if (resize)
        return;
    for (auto it = g_runtimes.begin(); it != g_runtimes.end();)
        it = it->second->native == native ? g_runtimes.erase(it) : std::next(it);
}

bool hdr_output(effect_runtime *runtime)
{
    const auto it = g_swapchains.find(runtime->get_native());
    if (it == g_swapchains.end())
        return false;
    const color_space cs = it->second->get_color_space();
    return cs == color_space::scrgb || cs == color_space::hdr10_pq || cs == color_space::hdr10_hlg;
}

void on_destroy_device(device *dev)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_runtimes.begin(); it != g_runtimes.end();)
        it = it->second->dev == dev ? g_runtimes.erase(it) : std::next(it);
}

void on_set_current_preset_path(effect_runtime *runtime, const char *path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (RuntimeData *rd = runtime_data(runtime))
        set_preset_path(*rd, path);
}

// Loads librashader, the capture shader and the preset, as needed. Returns false
// (with rd.error set) when there is nothing to render with.
bool prepare(RuntimeData &rd, ID3D11Device *d3d)
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
            set_error(rd, err);
            return false;
        }
    }
    if (!rd.renderer_ready)
    {
        if (!rd.renderer.init(d3d, err))
        {
            set_error(rd, err);
            return false;
        }
        rd.renderer_ready = true;
    }
    if (rd.force_reload || rd.chain_source != rd.companion || rd.chain_mtime != rd.companion_mtime ||
        (rd.retry && !rd.chain.ready()))
    {
        rd.force_reload = false;
        rd.retry = false;
        rd.chain_source = rd.companion;
        rd.chain_mtime = rd.companion_mtime;
        rd.chain_error.clear();
        const std::string name = utf8_from_path(rd.companion);
        const auto t0 = std::chrono::steady_clock::now();
        if (rd.chain.create(d3d, name, err))
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            log_info("Loaded " + name + " (" + std::to_string(int(ms)) + " ms)");
        }
        else
        {
            rd.chain_error = err;
            rd.logged_error = err; // logged here with the file name
            log_error("Could not load " + name + ": " + err);
        }
    }
    if (!rd.chain.ready())
    {
        rd.error = rd.chain_error;
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
        set_error(*rd, "Only Direct3D 11 games are supported for now.");
        return;
    }
    if (hdr_output(runtime))
    {
        set_error(*rd, "HDR output is not supported yet. Turn HDR off in the game to use RetroArch shaders.");
        return;
    }
    auto *d3d = reinterpret_cast<ID3D11Device *>(dev->get_native());
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
    if (!prepare(*rd, d3d))
        return;

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
    if (rd->renderer.snapshot(ctx, target, err) == nullptr)
    {
        set_error(*rd, err);
        return;
    }

    PixelGrid grid;
    if (g_settings.mode == native_auto)
    {
        // The detector reads the snapshot back in on_reshade_present, on the immediate
        // context: this event can come with a deferred command list (when another
        // add-on renders effects), where reading back is impossible.
        rd->detect_pending = true;
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

    if (!rd->renderer.render(ctx, grid, rd->chain, target, rd->frame++, err))
    {
        set_error(*rd, err);
        return;
    }
    rd->error.clear();
    rd->status = "Running on " + grid.describe() + (rd->detector.provisional() ? " (re-checking)" : "");
}

// Runs inside Present, on the immediate context, after ReShade has rendered effects
// and restored the game's pipeline state.
void on_reshade_present(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    RuntimeData *rd = runtime_data(runtime);
    if (rd == nullptr || !rd->companion_exists)
        return;
    const double t = now_seconds();

    if (rd->detect_pending && rd->renderer.snapshot_texture() != nullptr)
    {
        rd->detect_pending = false;
        auto *d3d = reinterpret_cast<ID3D11Device *>(runtime->get_device()->get_native());
        auto *imm = reinterpret_cast<ID3D11DeviceContext *>(
            runtime->get_command_queue()->get_immediate_command_list()->get_native());
        rd->detector.tick(d3d, imm, rd->renderer.snapshot_texture(), t);
    }

    // ReShade only calls reshade_begin_effects when it has effects to render. Explain
    // when that is why nothing happens.
    if (t - rd->last_effects > 2.0 && t - rd->created > 5.0)
    {
        rd->status = "Paused: ReShade is not rendering effects. They may be switched off (Effect toggle key), or no "
                     "effect files are installed; keep reshade-shaders\\Shaders\\RetroArchShaders.fx.";
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

// Relative references for shaders inside this game's folder (or the add-on's), so
// the folder can move; absolute ones for shared places such as a RetroArch
// install, so the .slangp still works when copied to another game.
bool reference_relative(const fs::path &target)
{
    auto key = [](const fs::path &p) {
        std::error_code ec;
        const fs::path c = fs::weakly_canonical(p, ec);
        std::wstring s = (ec ? p : c).wstring();
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };
    const std::wstring t = key(target);
    for (const fs::path &dir : {g_exe_dir, g_addon_dir})
    {
        std::wstring d = key(dir);
        if (!d.empty() && d.back() != L'\\')
            d += L'\\';
        if (t.compare(0, d.size(), d) == 0)
            return true;
    }
    return false;
}

std::string companion_description(const RuntimeData &rd)
{
    fs::path target;
    if (read_reference(rd.companion, target))
        return utf8_from_path(target.filename()) + "  (" + utf8_from_path(target.parent_path()) + ")";
    return utf8_from_path(rd.companion.filename());
}

void draw_shader_picker(RuntimeData &rd)
{
    if (!g_scan_started)
        start_scan();
    const bool scanning = poll_scan();

    if (scanning)
        ImGui::TextDisabled("Looking for shader presets...");
    else if (g_presets.empty())
    {
        ImGui::TextWrapped("No RetroArch shaders found. Put shader presets (for example the libretro "
                           "\"slang-shaders\" collection) in:");
        ImGui::TextUnformatted(utf8_from_path(g_addon_dir / L"retroarch-shaders").c_str());
        if (!g_roots.empty())
        {
            ImGui::TextDisabled("Searched:");
            for (const ShaderRoot &r : g_roots)
                ImGui::TextDisabled("  %s", utf8_from_path(r.dir).c_str());
        }
    }
    else
        ImGui::Text("%d presets in %d folder(s).", int(g_presets.size()), int(g_roots.size()));
    if (!scanning && g_scan_skipped > 0)
        ImGui::TextDisabled("%d folder(s) could not be read and were skipped.", g_scan_skipped);
    if (!scanning && !g_scan_error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Scan failed: %s", g_scan_error.c_str());

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Filter, e.g. crt-royale", g_filter, sizeof(g_filter));

    if (ImGui::BeginListBox("##presets", ImVec2(-1.0f, 260.0f)))
    {
        std::string filter = g_filter;
        for (char &c : filter)
            c = char(tolower(static_cast<unsigned char>(c)));
        for (int i = 0; i < int(g_presets.size()); ++i)
        {
            const std::string &label = g_presets[size_t(i)].label;
            std::string lower = label;
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
        const fs::path &target = g_presets[size_t(g_selected)].path;
        if (write_companion(rd.companion, target, reference_relative(target), {}, err))
        {
            rd.action_error.clear();
            refresh_companion(rd);
            rd.force_reload = true;
        }
        else
            set_action_error(rd, err);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!rd.companion_exists);
    if (ImGui::Button("Remove from this ReShade preset"))
    {
        // Set aside, not deleted: it may have been written by hand.
        std::string err;
        if (set_aside(rd.companion, err))
            rd.action_error.clear();
        else
            set_action_error(rd, err);
        refresh_companion(rd);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(scanning);
    if (ImGui::Button("Rescan"))
        start_scan();
    ImGui::EndDisabled();
}

void draw_resolution(RuntimeData &rd)
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
        save_settings();
}

// Writes the parameters that differ from what the referenced preset gives them, so
// values saved earlier are kept and untouched ones stay out of the file.
void save_parameters(RuntimeData &rd, const fs::path &target)
{
    std::vector<ShaderParam> base;
    std::string err;
    if (!read_preset_params(utf8_from_path(target), base, err))
    {
        set_action_error(rd, "Not saved: could not read " + utf8_from_path(target) + ": " + err);
        return;
    }
    if (!write_companion(rd.companion, target, reference_relative(target), param_overrides(rd.chain.params(), base),
                         err))
    {
        set_action_error(rd, "Not saved: " + err);
        return;
    }
    rd.action_error.clear();
    refresh_companion(rd);
}

void draw_parameters(RuntimeData &rd)
{
    // Only the chain compiled from this preset's current file (not a stale one).
    if (!rd.chain.ready() || rd.chain_source != rd.companion || rd.chain_mtime != rd.companion_mtime)
    {
        ImGui::TextDisabled("No shader loaded.");
        return;
    }
    const std::vector<ShaderParam> params = rd.chain.params();
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
            if (!rd.chain.set_param(p.name, v, err))
                set_action_error(rd, p.name + ": " + err);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s (saved value %g)", p.name.c_str(), p.initial);
        ImGui::PopID();
    }

    fs::path target;
    const bool is_reference = read_reference(rd.companion, target);
    ImGui::BeginDisabled(!is_reference);
    if (ImGui::Button("Save to this ReShade preset"))
        save_parameters(rd, target);
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

    ImGui::Text("ReShade preset: %s", utf8_from_path(rd->reshade_preset.filename()).c_str());
    if (rd->companion_exists)
        ImGui::TextWrapped("RetroArch shader: %s", companion_description(*rd).c_str());
    else
        ImGui::TextDisabled("No RetroArch shader for this ReShade preset.");
    if (!rd->action_error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", rd->action_error.c_str());
    if (!rd->error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", rd->error.c_str());
    else if (!rd->status.empty())
        ImGui::TextWrapped("%s", rd->status.c_str());
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Shader", ImGuiTreeNodeFlags_DefaultOpen))
        draw_shader_picker(*rd);
    if (ImGui::CollapsingHeader("Game resolution"))
        draw_resolution(*rd);
    if (ImGui::CollapsingHeader("Shader parameters"))
        draw_parameters(*rd);
}
} // namespace

// ReShade calls AddonInit/AddonUninit outside DllMain (not under the loader lock),
// which matters here: tearing down joins the grid detectors' and the scan's threads.
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
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
    reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(on_set_current_preset_path);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::register_overlay("RetroArch Shaders", draw_overlay);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE)
{
    reshade::unregister_overlay("RetroArch Shaders", draw_overlay);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_runtimes.clear(); // joins detector threads, frees chains
        g_swapchains.clear();
    }
    stop_scan();
    reshade::unregister_addon(addon_module);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
