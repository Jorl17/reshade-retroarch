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

// Target of the first #reference line, resolved against the companion's folder.
bool read_reference(const std::filesystem::path &companion, std::filesystem::path &target);

// Writes `companion` as a reference to `target` plus parameter values.
bool write_companion(const std::filesystem::path &companion, const std::filesystem::path &target,
                     const std::vector<std::pair<std::string, float>> &params, std::string &error);
