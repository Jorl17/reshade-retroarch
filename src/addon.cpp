// The ReShade add-on itself, built as RetroArchShaders.addon64, which ReShade loads into the
// game. Every frame it runs a RetroArch shader preset (a .slangp file, compiled and run by the
// librashader library) on the game's picture at its original low resolution. The preset is the
// "companion" .slangp: "<name>.slangp" next to the selected ReShade preset "<name>.ini" (see
// companion.h). This file handles ReShade's events, the settings and the add-on's overlay
// window; the GPU work is done by FrameRenderer (frame_renderer.h), through ReShade's API.

// ReShade's overlay header requires Dear ImGui's texture handle type to be 64 bits wide;
// this must be defined before imgui.h is included.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "companion.h"
#include "detector.h"
#include "discovery.h"
#include "librashader_api.h"
#include "frame_renderer.h"
#include "utf8.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

// Name and description ReShade shows for this add-on in its Add-ons list.
extern "C" __declspec(dllexport) const char *NAME = "RetroArch Shaders";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Runs RetroArch shader presets (.slangp) on the game's native pixels, through librashader.";

namespace fs = std::filesystem;
using namespace reshade::api;

namespace
{
// Section of ReShade.ini that holds this add-on's settings.
constexpr const char *kSection = "RetroArchShaders";

// How the add-on finds the game picture inside the frame and its original resolution (called
// "native" in the code): the "Game resolution" choice in the overlay, stored in ReShade.ini as
// NativeMode. The result is a "pixel grid" (PixelGrid in grid_detect.h): the rectangle of the
// frame the game picture covers, and how many original pixels it has across and down.
enum NativeMode : int
{
    native_auto = 0,   // "Detect automatically": GridDetector (detector.h) finds the grid
    native_manual = 1, // "Fixed": the whole frame, as manual_w x manual_h original pixels
    native_frame = 2,  // "Whole frame": nothing is recovered; the preset gets the full frame as is
};

// Settings shared by every game window, stored in the global ReShade.ini, section
// [RetroArchShaders]. Read by load_settings() and written by save_settings().
struct Settings
{
    int mode = native_auto;             // a NativeMode value
    int manual_w = 320, manual_h = 240; // original resolution used in "Fixed" mode
    std::vector<fs::path> extra_paths;  // extra folders to search for presets (ShaderPaths=)
};

// Everything the add-on keeps for one ReShade effect runtime. An effect runtime is ReShade's
// object that renders effects onto one swap chain (the set of images a window shows on
// screen; usually one per game window). The entry is kept when the swap chain is resized, so
// the compiled shader and the detected grid survive a window resize; it is removed when the
// swap chain or the graphics device is really destroyed.
struct RuntimeData
{
    // Graphics device of the runtime; on_destroy_device removes the entry when it goes away.
    device *dev = nullptr;
    // The swap chain's native handle (the IDXGISwapChain pointer): the key into g_swapchains,
    // and how on_destroy_swapchain finds the entry.
    uint64_t native = 0;
    // The selected ReShade preset (.ini) and the path of its companion .slangp.
    fs::path reshade_preset, companion;
    bool companion_exists = false;        // whether the companion file existed at the last check
    fs::file_time_type companion_mtime{}; // its last-modified time at the last check

    FrameRenderer renderer;      // copies the frame, extracts the low resolution picture, runs the chain
    bool renderer_ready = false; // renderer.init() succeeded
    ShaderChain chain;           // the companion preset compiled by librashader (chain.h)
    // Which companion file `chain` was last compiled from, and that file's modification time
    // then. Recorded even when compiling failed.
    fs::path chain_source;
    fs::file_time_type chain_mtime{};
    std::string chain_error; // why the last compile failed; empty if it worked

    GridDetector detector; // finds the pixel grid in copies of the frame, in the background
    // on_begin_effects took a snapshot (a copy of the frame) that on_reshade_present has not
    // yet given to the detector.
    bool detect_pending = false;
    uint64_t frame = 0; // frames rendered; passed to the shaders, which may animate with it
    // Times in seconds (now_seconds()): next on-disk check of the companion file, last time
    // on_begin_effects ran, and last time the runtime was initialised or reloaded its effects.
    double next_file_check = 0, last_effects = 0, created = 0;
    bool force_reload = false; // recompile even if unchanged (Revert, Use buttons)
    // Set when the companion file appears, disappears or changes on disk: if no chain is
    // loaded, prepare() compiles again even though the file and time match the last attempt.
    bool retry = false;
    bool warned_paused = false; // the "Paused" status was logged; reset when effects run again
    // Shown in the overlay: the current error in red if there is one, otherwise the status line.
    std::string status, error;
    std::string logged_error; // the last error written to ReShade.log, so each one is logged once
    // Error from an overlay button or slider; stays until a later action succeeds or another
    // ReShade preset is selected.
    std::string action_error;
};

// One background search for RetroArch presets: the thread finds the shader folders
// (find_shader_roots) and every .slangp in them (scan_presets), both in discovery.h, then sets
// `done`. Setting `cancel` asks it to stop early. start_scan() creates it and poll_scan() takes
// the results. It is only ever reached through the g_scan pointer and never destroyed by a
// static destructor, so if the game exits without unloading add-ons it is simply leaked:
// destroying a still-running std::thread in a static destructor terminates or hangs the process.
struct Scan
{
    std::atomic<bool> cancel{false}, done{false};
    std::vector<ShaderRoot> roots;    // folders searched
    std::vector<PresetEntry> presets; // presets found, sorted by label
    int skipped = 0;                  // folders that could not be read
    std::string error;                // set if the search threw an exception
    std::thread thread;
};

HMODULE g_module = nullptr; // this add-on's DLL, set in AddonInit
// Folder of the add-on DLL and folder of the game's .exe: both are searched for a
// "retroarch-shaders" folder, and presets inside them are referenced with relative paths.
fs::path g_addon_dir, g_exe_dir;
Settings g_settings;
bool g_settings_loaded = false; // load_settings() already ran

// Taken by every ReShade event handler below and by the overlay, which ReShade may call from
// different threads. It guards g_runtimes, g_swapchains and the RuntimeData they hold.
std::mutex g_mutex;
// The add-on's data for each ReShade effect runtime.
std::unordered_map<effect_runtime *, std::unique_ptr<RuntimeData>> g_runtimes;
// Every live swap chain, by native handle. The effect runtime does not report its colour
// space, so hdr_output() looks up the runtime's swap chain here to read it.
std::unordered_map<uint64_t, swapchain *> g_swapchains;

// Preset search state, used only by the overlay (and by AddonUninit): the running search
// (nullptr if none), the results of the last finished one, and whether one was ever started.
Scan *g_scan = nullptr;
std::vector<ShaderRoot> g_roots;
std::vector<PresetEntry> g_presets;
int g_scan_skipped = 0;
std::string g_scan_error;
bool g_scan_started = false;

// Overlay state: the text in the filter box, and the index into g_presets of the preset
// selected in the list (-1 for none).
char g_filter[128] = "";
int g_selected = -1;

// Returns the current time in seconds from a clock that never jumps. Only differences
// between two values mean anything.
double now_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Write `s` to ReShade.log as an information line or as an error line.
void log_info(const std::string &s) { reshade::log::message(reshade::log::level::info, s.c_str()); }
void log_error(const std::string &s) { reshade::log::message(reshade::log::level::error, s.c_str()); }

// Returns `s` without its leading and trailing spaces and tabs.
std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Reads the settings from the global ReShade.ini into g_settings. Only the first call does
// anything. Out-of-range values are corrected: an unknown mode becomes automatic detection,
// and the "Fixed" size is clamped to 16..7680 x 16..4320.
// This and save_settings() use the global file (runtime argument nullptr), not a runtime's
// own configuration: writing to a runtime's configuration makes ReShade reload all of it,
// which can switch presets.
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

    // ShaderPaths: extra folders to search for presets. The first call asks for the value's
    // size. ReShade splits values on ',' and returns the parts separated by '\0', so '\0',
    // ';' and ',' all separate folders here.
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

// Writes the resolution mode and the "Fixed" width and height from g_settings to the global
// ReShade.ini. ShaderPaths is never written: only the user edits it.
void save_settings()
{
    reshade::set_config_value(nullptr, kSection, "NativeMode", g_settings.mode);
    reshade::set_config_value(nullptr, kSection, "NativeWidth", g_settings.manual_w);
    reshade::set_config_value(nullptr, kSection, "NativeHeight", g_settings.manual_h);
}

// Starts a background search for shader presets (see Scan), unless one is already running.
// The results reach g_roots and g_presets once poll_scan() sees the search has finished.
void start_scan()
{
    if (g_scan != nullptr)
        return; // one at a time
    g_scan_started = true;
    Scan *scan = new Scan();
    // The thread gets its own copies of the folders, so it never reads globals.
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

// Checks on the background preset search. If it has finished: waits for its thread to end,
// moves its results into g_roots, g_presets, g_scan_skipped and g_scan_error (logging a
// failure), frees it, and clears the list selection, whose index would now point at a
// different preset. Returns true while a search is still running, false otherwise.
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

// Asks the running preset search, if any, to stop, waits for its thread to end and frees it,
// discarding its results. Used when the add-on unloads.
void stop_scan()
{
    if (g_scan == nullptr)
        return;
    g_scan->cancel = true;
    g_scan->thread.join();
    delete g_scan;
    g_scan = nullptr;
}

// Checks on disk whether rd.companion exists and when it was last modified. If either
// changed since the last check, stores the new values, sets rd.retry and clears rd.error.
// A new modification time makes the next frame compile the file again (see prepare()); a
// missing file makes the next frame show the game untouched.
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

// Records that the ReShade preset at `path` (UTF-8; nullptr or empty for none) is selected.
// If it is the preset already recorded, it only re-checks the companion file, keeping the
// compiled shader. Otherwise it switches rd to that preset's companion .slangp (same path,
// .slangp extension), clears the button error, and checks whether the file exists; if it
// does, the next frame compiles it.
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

// Asks ReShade which preset `runtime` has selected and, if it is not the one recorded in rd,
// switches to it (set_preset_path). Called every frame: ReShade reports preset switches made
// with its keys and overlay through an event (on_set_current_preset_path), but not ones made
// by other add-ons through its API, and this check is cheap.
void poll_preset_path(effect_runtime *runtime, RuntimeData &rd)
{
    char path[4096] = "";
    size_t size = sizeof(path);
    runtime->get_current_preset_path(path, &size);
    if (utf8_from_path(rd.reshade_preset) != path)
        set_preset_path(rd, path);
}

// Returns the add-on's data for `runtime`, or nullptr if it has none (not initialised yet,
// or already destroyed). The caller must hold g_mutex.
RuntimeData *runtime_data(effect_runtime *runtime)
{
    const auto it = g_runtimes.find(runtime);
    return it == g_runtimes.end() ? nullptr : it->second.get();
}

// Sets rd.error, which the overlay shows in red, and writes it to ReShade.log unless it is
// the error logged last, so an error that repeats every frame is logged once. The log
// matters because the overlay is closed most of the time.
void set_error(RuntimeData &rd, const std::string &error)
{
    rd.error = error;
    if (error != rd.logged_error)
        log_error("RetroArch Shaders: " + error);
    rd.logged_error = error;
}

// Sets rd.action_error, which the overlay shows in red until a later action succeeds, and
// writes it to ReShade.log. Used for failures of the overlay's buttons and sliders, which
// happen once per click, so each is always logged.
void set_action_error(RuntimeData &rd, const std::string &error)
{
    rd.action_error = error;
    log_error("RetroArch Shaders: " + error);
}

// ReShade event: an effect runtime was initialised, after its swap chain was created or
// resized. Creates the add-on's data for it (or reuses the data kept from before a resize),
// records its device and swap chain handle, and picks up the selected ReShade preset. The
// first call also loads the settings.
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

// ReShade event: an effect runtime is being reset (its swap chain is resized) or destroyed.
// Makes the grid detector finish with the frame copy it may be reading, and free the
// staging texture (a GPU texture the CPU can read) that holds it; that must happen while the
// device context still exists. The rest of the runtime's data (the detected grid, the
// compiled shader) is kept for when the runtime is initialised again; on_destroy_swapchain
// and on_destroy_device remove it when the swap chain or device really goes away.
void on_destroy_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    RuntimeData *rd = runtime_data(runtime);
    if (rd == nullptr)
        return;
    rd->detector.shutdown(runtime->get_device());
    rd->detect_pending = false;
}

// ReShade event: ReShade reloaded all its effect files. Restarts the grace period before the
// "Paused" status (see on_reshade_present), since no effects are rendered while they load.
void on_reloaded_effects(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (RuntimeData *rd = runtime_data(runtime))
        rd->created = now_seconds();
}

// ReShade event: a swap chain was created or resized. Records it in g_swapchains under its
// native handle, so hdr_output() can find it and read its colour space.
void on_init_swapchain(swapchain *sc, bool)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_swapchains[sc->get_native()] = sc;
}

// ReShade event: a swap chain is being resized (`resize` true) or destroyed. Removes it from
// g_swapchains; after a resize, on_init_swapchain adds it back. When it is destroyed, also
// removes the data of its effect runtime. ReShade raises this after destroy_effect_runtime
// and deletes the runtime object right after it, so the data has to go now: a later runtime
// may even get the same address and must not inherit it.
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

// Returns true if the swap chain of `runtime` outputs HDR (colour space scRGB, HDR10 PQ or
// HDR10 HLG), false if it outputs SDR or its swap chain is not known. The caller must hold
// g_mutex.
bool hdr_output(effect_runtime *runtime)
{
    const auto it = g_swapchains.find(runtime->get_native());
    if (it == g_swapchains.end())
        return false;
    const color_space cs = it->second->get_color_space();
    return cs == color_space::scrgb || cs == color_space::hdr10_pq || cs == color_space::hdr10_hlg;
}

// ReShade event: a graphics device is being destroyed. Removes the data of every effect
// runtime on that device, which also frees the GPU resources it created on it.
void on_destroy_device(device *dev)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_runtimes.begin(); it != g_runtimes.end();)
    {
        if (it->second->dev != dev)
        {
            ++it;
            continue;
        }
        it->second->detector.shutdown(dev);
        it = g_runtimes.erase(it);
    }
}

// ReShade event: a ReShade preset was loaded, because the user picked another one or effects
// were reloaded. Switches `runtime` to that preset's companion .slangp (set_preset_path).
void on_set_current_preset_path(effect_runtime *runtime, const char *path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (RuntimeData *rd = runtime_data(runtime))
        set_preset_path(*rd, path);
}

// Gets everything ready to render for rd on device `dev`, doing only what is not
// done yet: loads librashader.dll from the add-on's folder, sets up the renderer, and
// compiles the companion .slangp into rd.chain when it is new, changed on disk, or a button
// asked for it. Returns true when a compiled shader is ready; false when there is nothing
// to render with, normally with rd.error saying why.
bool prepare(RuntimeData &rd, device *dev)
{
    std::string err;
    ChainDevice chain_device;
    if (!FrameRenderer::chain_device(dev, chain_device, err))
    {
        set_error(rd, err); // this graphics API is not supported (yet)
        return false;
    }
    if (!libra::loaded())
    {
        // After a failed load, try again at most every 2 seconds.
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
        if (!rd.renderer.init(dev, err))
        {
            set_error(rd, err);
            return false;
        }
        rd.renderer_ready = true;
    }
    // Compile when a button asked for it, when the companion is a different file or version
    // than the last attempt, or when it changed on disk and no chain is loaded. The attempt
    // is recorded before compiling, so a preset that fails is not recompiled every frame.
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
        if (rd.chain.create(chain_device, name, err))
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

// ReShade event, once per frame, right before ReShade renders its own effects: this is where
// the RetroArch shader runs. `rtv` is a view of the back buffer, the image the game has just
// drawn and is about to show; the shader's result is written back into it, and ReShade's own
// effects then run on top. `cmd_list` is the command list to record the work on.
// The frame is left untouched when the ReShade preset has no companion .slangp, the game's
// graphics API is not supported, the output is HDR, the shader cannot be loaded, or (in
// automatic mode) no pixel grid has been found yet. Sets rd's status or error to say what
// happened.
void on_begin_effects(effect_runtime *runtime, command_list *cmd_list, resource_view rtv, resource_view)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    RuntimeData *rd = runtime_data(runtime);
    if (rd == nullptr)
        return;
    const double t = now_seconds();
    rd->last_effects = t;
    poll_preset_path(runtime, *rd);
    // Once a second, check the companion file on disk.
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
    if (hdr_output(runtime))
    {
        set_error(*rd, "HDR output is not supported yet. Turn HDR off in the game to use RetroArch shaders.");
        return;
    }
    if (!prepare(*rd, dev))
        return;

    // The back buffer texture behind `rtv`, with its size and format.
    const resource target = dev->get_resource_from_view(rtv);
    if (target.handle == 0)
        return;
    const resource_desc desc = dev->get_resource_desc(target);

    // Take a snapshot: a copy of the frame as the game drew it, made before anything draws
    // over it. The renderer reads the low resolution picture from it, and the grid detector
    // analyses it.
    std::string err;
    if (!rd->renderer.snapshot(cmd_list, target, err))
    {
        set_error(*rd, err);
        return;
    }

    // Choose the pixel grid for the current mode: where the game picture sits in the frame
    // and its original resolution.
    PixelGrid grid;
    if (g_settings.mode == native_auto)
    {
        // Automatic: use the grid the detector has found so far, and have it analyse this
        // snapshot later, in on_reshade_present. The detector reads the snapshot back from the
        // GPU on the immediate command list (the one ReShade executes at present). This event
        // can instead come with another command list (on Direct3D 11, a deferred context that
        // only records commands for later) when another add-on renders effects.
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
        // Fixed and Whole frame: the grid covers the whole frame. Fixed divides it into the
        // configured number of original pixels; Whole frame uses one per frame pixel.
        grid.valid = true;
        grid.rect_w = int(desc.texture.width);
        grid.rect_h = int(desc.texture.height);
        grid.native_w = g_settings.mode == native_manual ? std::max(1, g_settings.manual_w) : int(desc.texture.width);
        grid.native_h = g_settings.mode == native_manual ? std::max(1, g_settings.manual_h) : int(desc.texture.height);
        grid.match = 1.0f;
    }

    // Extract the low resolution picture, run the preset on it and write the result over the
    // grid's rectangle of the back buffer. Pixels outside the rectangle are left as they are.
    if (!rd->renderer.render(cmd_list, grid, rd->chain, target, rd->frame++, err))
    {
        set_error(*rd, err);
        return;
    }
    rd->error.clear();
    // "(re-checking)": the frame size changed, and the detector is still confirming the grid
    // it rescaled from the old size.
    rd->status = "Running on " + grid.describe() + (rd->detector.provisional() ? " (re-checking)" : "");
}

// ReShade event, once per frame, after ReShade has drawn its effects and its overlay, just
// before the frame is shown. Gives the grid detector the snapshot that on_begin_effects took
// (the detector copies it and reads it back on the immediate command list, see detector.h), and
// sets the "Paused" status when on_begin_effects has stopped being called.
void on_reshade_present(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    RuntimeData *rd = runtime_data(runtime);
    if (rd == nullptr || !rd->companion_exists)
        return;
    const double t = now_seconds();

    if (rd->detect_pending && rd->renderer.snapshot_resource().handle != 0)
    {
        rd->detect_pending = false;
        rd->detector.tick(runtime->get_device(), runtime->get_command_queue(), rd->renderer.snapshot_resource(), t);
    }

    // ReShade only calls reshade_begin_effects when it has effects to render: effects are
    // switched on and at least one effect file is loaded (the add-on ships a hidden
    // placeholder, RetroArchShaders.fx, for that). If it has not been called for 2 seconds,
    // and the runtime has had 5 seconds to load its effects, explain why nothing happens,
    // and log that once.
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
// Overlay: the "RetroArch Shaders" window inside ReShade's overlay (opened with the Home
// key), built with Dear ImGui, the UI library ReShade uses: the window is redrawn from
// scratch every frame, and each widget call returns true when the user has just used it.
// Everything below runs from draw_overlay(), which holds g_mutex.

// Returns true if the preset file `target` is inside the game's folder or the add-on's
// folder (compared case-insensitively, with links and ".." resolved where possible). The
// companion .slangp then refers to it with a relative path, so the folder can be moved
// with its shaders. Presets in shared places, such as a RetroArch install, get an absolute
// path instead, so the .slangp still works when copied to another game.
bool reference_relative(const fs::path &target)
{
    // A path's comparable form: resolved as far as it exists, in lower case.
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
        // The trailing backslash keeps "C:\game2\x" from counting as inside "C:\game".
        std::wstring d = key(dir);
        if (!d.empty() && d.back() != L'\\')
            d += L'\\';
        if (t.compare(0, d.size(), d) == 0)
            return true;
    }
    return false;
}

// Returns the text the overlay shows for the active RetroArch shader: if the companion
// .slangp is a reference to another preset (a "#reference" line), that preset's file name
// and folder; otherwise the companion's own file name.
std::string companion_description(const RuntimeData &rd)
{
    fs::path target;
    if (read_reference(rd.companion, target))
        return utf8_from_path(target.filename()) + "  (" + utf8_from_path(target.parent_path()) + ")";
    return utf8_from_path(rd.companion.filename());
}

// Draws the overlay's "Shader" section: the presets found on disk, a box to filter them by
// name, and buttons to attach the selected preset to the current ReShade preset, detach the
// attached one, or search the folders again. The first call starts the search.
void draw_shader_picker(RuntimeData &rd)
{
    if (!g_scan_started)
        start_scan();
    const bool scanning = poll_scan();

    // Summary line: still searching; nothing found, with where to put shaders and which
    // folders were searched; or how many presets were found. Then any search problems.
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

    // The list shows the presets whose label contains the filter text, ignoring case.
    // Clicking one selects it. PushID gives each row its own ImGui ID even if labels repeat.
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

    // "Use": write the companion .slangp as a reference to the selected preset, with no
    // parameter changes, and compile it on the next frame. Needs a selection and a
    // selected ReShade preset.
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
    // "Remove": detach the RetroArch shader from this ReShade preset. The companion file is
    // renamed out of the way (to "<name>.slangp.removed"), not deleted, because it may have
    // been written by hand. The next frame finds no companion and the shader turns off.
    ImGui::BeginDisabled(!rd.companion_exists);
    if (ImGui::Button("Remove from this ReShade preset"))
    {
        std::string err;
        if (set_aside(rd.companion, err))
            rd.action_error.clear();
        else
            set_action_error(rd, err);
        refresh_companion(rd);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // "Rescan": search the folders again (disabled while a search is running).
    ImGui::BeginDisabled(scanning);
    if (ImGui::Button("Rescan"))
        start_scan();
    ImGui::EndDisabled();
}

// Draws the overlay's "Game resolution" section: the three modes (see NativeMode), the
// width and height for "Fixed" (kept within 16..7680 x 16..4320), and in automatic mode the
// detector's state and a "Detect again" button that makes it forget the grid and search
// again. Writes the settings to ReShade.ini when any of them changed. The settings are
// global: they apply to every game window at once.
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

// Saves the current slider values into the companion .slangp. `target` is the preset the
// companion already refers to. The file is rewritten as a reference to `target` plus every
// parameter whose current value differs from the value `target` itself gives it: values
// saved earlier are kept (they still differ), and parameters left at the preset's value
// stay out of the file. The rewritten file has a new modification time, so the next frame
// compiles it again. On failure, sets rd.action_error.
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

// Draws the overlay's "Shader parameters" section: one slider per parameter of the running
// shader, which takes effect immediately, plus "Save to this ReShade preset" (see
// save_parameters) and "Revert", which compiles the file again and so drops unsaved changes.
void draw_parameters(RuntimeData &rd)
{
    // Show sliders only for a chain compiled from the companion file as it is on disk now;
    // otherwise they would belong to another shader or an outdated version of it.
    if (!rd.chain.ready() || rd.chain_source != rd.companion || rd.chain_mtime != rd.companion_mtime)
    {
        ImGui::TextDisabled("No shader loaded.");
        return;
    }
    const std::vector<ShaderParam> params = rd.chain.params();
    if (params.empty())
        ImGui::TextDisabled("This preset has no parameters.");
    // Parameters whose range is empty get no slider. A moved slider is snapped to the
    // parameter's step and applied to the chain. Hovering shows the parameter's internal
    // name and its saved value (the one the .slangp gives it, or the shader's default).
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

    // Saving rewrites the companion as a reference plus changed values, so it is only
    // offered when the companion is already a reference; a full preset is left alone.
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

// Draws the add-on's window in ReShade's overlay for `runtime`: the selected ReShade preset,
// the RetroArch shader attached to it, any error or the current status, and the "Shader",
// "Game resolution" and "Shader parameters" sections. ReShade calls it every frame while
// the overlay is open.
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

// Called by ReShade after it loads this DLL. Registers the add-on with ReShade (returns false,
// and the add-on is not used, if ReShade refuses it), remembers the add-on's and the game's
// folders, and subscribes the event handlers above and the overlay window.
// ReShade calls AddonInit and AddonUninit outside DllMain, so not under the Windows loader
// lock. That matters: AddonUninit waits for the grid detectors' and the scan's threads to
// end, which could deadlock under that lock.
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

// Called by ReShade before it unloads this DLL. Removes the overlay window, frees all
// per-runtime data (which stops the grid detectors' worker threads and frees the compiled
// shaders), stops a running preset search, and unregisters the add-on.
extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE)
{
    reshade::unregister_overlay("RetroArch Shaders", draw_overlay);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto &[runtime, rd] : g_runtimes)
            rd->detector.shutdown(rd->dev); // frees its GPU objects
        g_runtimes.clear(); // joins detector threads, frees renderers and chains
        g_swapchains.clear();
    }
    stop_scan();
    reshade::unregister_addon(addon_module);
}

// Standard Windows DLL entry point. Does nothing: set-up and tear-down happen in AddonInit
// and AddonUninit.
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
