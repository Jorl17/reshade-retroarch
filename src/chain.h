#pragma once
// Loads and runs RetroArch shader presets through librashader, a library that runs
// RetroArch's "slang" shaders outside RetroArch. A preset is a .slangp file: a list of
// shader passes (each a .slang shader file) applied one after another, plus values for
// the shaders' adjustable parameters. This file wraps librashader's compiled preset for
// Direct3D 11 (a "filter chain") in a class, and reads and compares preset parameters,
// which the add-on's window shows as sliders and saves.

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// One adjustable parameter of a preset, shown as a slider in the add-on's window.
// Shaders declare parameters with `#pragma parameter` lines; a preset can set their
// values.
struct ShaderParam
{
    // `name` identifies the parameter in .slangp files and librashader calls;
    // `description` is the label to show (the name when the shader gives none).
    std::string name, description;
    float value = 0;   // value in use now (ShaderChain::set_param changes it)
    float initial = 0; // value the preset gives it: set in the .slangp, or the shader's default
    // Slider range and step, as declared by the shader.
    float minimum = 0, maximum = 0, step = 0;
};

// Fills `out` with the parameters of the preset file at `preset_path` (a UTF-8 path),
// each with the value the preset gives it. Reads files only: nothing is compiled and
// no GPU is needed. Returns false and sets `error` if librashader is not loaded (see
// librashader_api.h) or the preset cannot be read.
bool read_preset_params(const std::string &preset_path, std::vector<ShaderParam> &out, std::string &error);

// Returns the parameters in `current` whose value differs from the value `base` gives
// them, as (name, value) pairs in `current`'s order; a parameter missing from `base` is
// always included, and values within 1e-6 count as equal.
// This is what a companion .slangp has to store. (A companion is the small .slangp the
// add-on writes next to a ReShade preset: a #reference line naming the preset the user
// picked, which is `base`, plus the parameters the user changed; see companion.h.)
// `current` normally comes from the chain compiled from the companion itself, so values
// saved there earlier still differ from `base` and are kept, and values moved back to
// the base value drop out.
std::vector<std::pair<std::string, float>> param_overrides(const std::vector<ShaderParam> &current,
                                                           const std::vector<ShaderParam> &base);

// A RetroArch preset compiled by librashader for one Direct3D 11 device, plus its
// parameters. create() loads and compiles a preset, frame() draws it, set_param()
// changes a parameter while it runs. Needs librashader loaded first (libra::load in
// librashader_api.h). The destructor frees the compiled preset.
class ShaderChain
{
public:
    ShaderChain() = default;
    ShaderChain(const ShaderChain &) = delete;
    ShaderChain &operator=(const ShaderChain &) = delete;
    ~ShaderChain() { destroy(); }

    // Loads the preset file at `preset_path` (a UTF-8 path) and compiles all its shader
    // passes for `device`. Any preset loaded before is freed first, even if this one then
    // fails. Returns false and sets `error` if librashader is not loaded or the preset
    // fails to load or compile; ready() is then false. Compiling runs on the calling
    // thread and can take a while for large presets.
    bool create(ID3D11Device *device, const std::string &preset_path, std::string &error);
    // Frees the compiled preset and forgets its parameters and path. Safe to call when
    // nothing is loaded.
    void destroy();
    // True when a preset is compiled and frame() can draw.
    bool ready() const { return chain_ != nullptr; }

    // Draws one frame of the preset: reads `input` (the image to process; in this
    // project, the game's native image) and draws the result into the area (x, y, w, h)
    // of `output`. `frame_count` is the frame number passed to the shaders; animated
    // effects (noise, flicker, interlacing) use it, so it should go up by one each frame.
    // The work is recorded on `ctx`. Returns false and sets `error` if no preset is
    // loaded or librashader reports an error.
    // Do not rely on the rest of `output` being kept: Renderer::render (renderer.h) gives
    // it a texture exactly the size of the area for that reason.
    bool frame(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *input, ID3D11RenderTargetView *output, int x,
               int y, int w, int h, uint64_t frame_count, std::string &error);

    // Sets parameter `name` of the loaded preset to `value`, from the next frame() on,
    // and updates it in params(). Returns false if no preset is loaded (leaving `error`
    // unchanged), or if librashader rejects the change, for example for an unknown name
    // (with `error` set).
    bool set_param(const std::string &name, float value, std::string &error);
    // The loaded preset's parameters with their current values; empty when nothing is
    // loaded.
    const std::vector<ShaderParam> &params() const { return params_; }
    // The path given to the create() call that succeeded; empty when nothing is loaded.
    const std::string &preset() const { return preset_; }

private:
    // librashader's handle to the compiled preset, or null. Stored as void* so this
    // header does not need librashader's headers.
    void *chain_ = nullptr; // a libra_d3d11_filter_chain_t
    std::vector<ShaderParam> params_;
    std::string preset_;
};
