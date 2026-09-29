#pragma once
// Offline Direct3D 12 rendering for render_png (tools/render_png.cpp): does what the add-on
// does to a frame on Direct3D 12 (FrameRenderer in src/frame_renderer.h), with plain
// Direct3D 12 instead of ReShade. The end-to-end test uses it as the expected result on
// Direct3D 12, because librashader's Direct3D 12 runtime samples some shaders slightly
// differently from its Direct3D 11 one (see docs/assumptions.md).

#include "grid_detect.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Renders `frames` frames of the preset at `preset_path` (UTF-8) on the picture `rgba`
// (`w` x `h` pixels, 8-bit RGBA, rows packed) like the add-on: rebuilds the native picture
// described by `grid` with the capture preset, runs the preset on it (with the parameter
// changes in `sets`) into an image the size of grid's rectangle, and writes that into
// `rgba` at the rectangle; the rest of `rgba` is unchanged. Uses the default GPU. The
// DirectX Shader Compiler (dxcompiler.dll, dxil.dll) is loaded from `dxc_dir`.
// librashader must already be loaded. Returns false and sets `error` on failure.
bool render_d3d12(std::vector<uint8_t> &rgba, UINT w, UINT h, const PixelGrid &grid, const std::string &preset_path,
                  const std::vector<std::pair<std::string, float>> &sets, int frames, const std::wstring &dxc_dir,
                  std::string &error);
