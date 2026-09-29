#pragma once
// Finding RetroArch shader presets (.slangp files) on the computer, for the list in the
// add-on's window: which folders to search (find_shader_roots) and every preset in them
// (scan_presets). The add-on runs both on a background thread (see Scan in addon.cpp).

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

// A folder searched for presets.
struct ShaderRoot
{
    std::string name;          // shown in the window, e.g. "RetroArch (Steam)"
    std::filesystem::path dir; // the folder
};

// One preset found in a ShaderRoot.
struct PresetEntry
{
    std::string label;          // shown in the list: "<root name>/<path inside the root>"
    std::filesystem::path path; // the .slangp file
};

// Returns the folders to search that exist on this computer, each once, in priority order:
//  1. "retroarch-shaders" next to the add-on (drop shader packs here),
//  2. the same next to the game executable, if that is elsewhere,
//  3. the slang shaders of a RetroArch installation (default install folder,
//     %APPDATA%, or any Steam library),
//  4. `extra` folders from the configuration.
std::vector<ShaderRoot> find_shader_roots(const std::filesystem::path &addon_dir,
                                          const std::filesystem::path &exe_dir,
                                          const std::vector<std::filesystem::path> &extra);

// Returns every .slangp file under the roots (subfolders included), sorted by label.
// Folders that cannot be opened are skipped and counted in `skipped`; linked folders
// (symbolic links, junctions) are followed, but no folder is visited twice. Stops early,
// returning what it has found so far, when `cancel` becomes true.
std::vector<PresetEntry> scan_presets(const std::vector<ShaderRoot> &roots, const std::atomic<bool> &cancel,
                                      int &skipped);
