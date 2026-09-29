#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

// Folders searched for RetroArch shader presets (.slangp).
struct ShaderRoot
{
    std::string name;          // shown in the UI
    std::filesystem::path dir;
};

struct PresetEntry
{
    std::string label;          // "<root name>/<path inside root>"
    std::filesystem::path path;
};

// Existing folders, in priority order:
//  1. "retroarch-shaders" next to the add-on (drop shader packs here),
//  2. the same next to the game executable, if that is elsewhere,
//  3. the slang shaders of a RetroArch installation (default install folder,
//     %APPDATA%, or any Steam library),
//  4. `extra` folders from the configuration.
std::vector<ShaderRoot> find_shader_roots(const std::filesystem::path &addon_dir,
                                          const std::filesystem::path &exe_dir,
                                          const std::vector<std::filesystem::path> &extra);

// Every .slangp under the roots, sorted by label. Folders that cannot be opened
// are skipped (and counted in `skipped`); linked folders (symlinks, junctions) are
// followed once. Stops early, returning what it has, when `cancel` becomes true.
std::vector<PresetEntry> scan_presets(const std::vector<ShaderRoot> &roots, const std::atomic<bool> &cancel,
                                      int &skipped);
