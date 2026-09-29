#pragma once
// Access to the capture preset: the one-pass RetroArch shader preset (capture.slangp,
// shader capture.slang) that rebuilds a game's native, low-resolution picture from the
// frame the game presented. It runs through librashader like any other preset, so the
// same capture code works on every graphics API librashader supports.

#include <string>

// Returns the path (UTF-8) of capture.slangp, ready for librashader to load. Both files
// are built into the program; librashader only loads presets from files, so the first
// call writes them into a folder under the user's temporary directory (named after
// their content, so different versions never mix) and later calls return the same
// path. Returns an empty string and sets `error` if the files cannot be written.
std::string capture_preset_path(std::string &error);
