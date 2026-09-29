#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// A RetroArch preset attached to a ReShade preset: "<ReShade preset>.slangp" next
// to "<ReShade preset>.ini". While that ReShade preset is selected, the add-on
// runs the attached RetroArch preset. It is an ordinary .slangp, so it can be a
// full preset or (as the add-on writes them) a reference plus parameter changes:
//
//   #reference "../retroarch-shaders/crt/crt-royale.slangp"
//   diffusion_weight = "0.004688"

std::filesystem::path companion_path(const std::filesystem::path &reshade_preset);

// Target of the first #reference line, resolved the way librashader resolves it
// (against the companion's folder with symlinks and junctions resolved).
bool read_reference(const std::filesystem::path &companion, std::filesystem::path &target);

// Writes `companion` as a reference to `target` plus parameter values. The
// reference is relative when `relative` is set and possible (so a game folder can
// be moved or copied with its shaders), absolute otherwise (for shaders in shared
// places such as a RetroArch install, so the file can be copied to other games).
bool write_companion(const std::filesystem::path &companion, const std::filesystem::path &target, bool relative,
                     const std::vector<std::pair<std::string, float>> &params, std::string &error);

// Renames `file` out of the way to "<file>.removed" (or ".removed.2", ... if that
// exists), so nothing is overwritten. Returns false with `error` set on failure.
bool set_aside(const std::filesystem::path &file, std::string &error);
