// Implementation of chain.h: the calls into librashader that load RetroArch presets,
// read their parameters, and compile and run them on Direct3D 11. librashader's
// functions are reached through libra::api() (librashader_api.h), a table of function
// pointers filled when the DLL is loaded at runtime.

#include "chain.h"
#include "librashader_api.h"

#include <cmath>

namespace
{
// Fills `out` with the parameters of `preset` (a preset already loaded with
// librashader's preset_create), each with the value the preset gives it: the value
// set in the .slangp if any, otherwise the shader's default. Returns false and sets
// `error` if librashader cannot list them.
// A .slangp can start with `#reference "other.slangp"` to build on another preset and
// then set some values itself, and the referenced file can do the same. When several
// files in that chain set a parameter, the value read last wins (the referencing file's
// own lines come after its #reference), as when the preset runs.
// (libra_preset_get_param is not used: it returns the first value set, and nothing at
// all for parameters the preset does not set.)
bool read_params(libra_shader_preset_t &preset, std::vector<ShaderParam> &out, std::string &error)
{
    const libra_instance_t &api = libra::api();
    out.clear();
    libra_preset_param_list_t list = {};
    if (libra_error_t err = api.preset_get_runtime_params(&preset, &list))
    {
        error = libra::take_error(err);
        return false;
    }
    for (uint64_t i = 0; i < list.length; ++i)
    {
        const libra_preset_param_t &p = list.parameters[i];
        ShaderParam sp;
        sp.name = p.name ? p.name : "";
        sp.description = p.description ? p.description : sp.name;
        sp.initial = p.initial;
        sp.value = p.initial;
        sp.minimum = p.minimum;
        sp.maximum = p.maximum;
        sp.step = p.step;
        out.push_back(sp);
    }
    api.preset_free_runtime_params(list);
    return true;
}
} // namespace

// Loads the preset (without compiling it), reads its parameters into `out` and frees
// it. Returns false and sets `error` on failure.
bool read_preset_params(const std::string &preset_path, std::vector<ShaderParam> &out, std::string &error)
{
    if (!libra::loaded())
    {
        error = "librashader is not loaded";
        return false;
    }
    const libra_instance_t &api = libra::api();
    libra_shader_preset_t preset = nullptr;
    if (libra_error_t err = api.preset_create(preset_path.c_str(), &preset))
    {
        error = libra::take_error(err);
        return false;
    }
    const bool ok = read_params(preset, out, error);
    api.preset_free(&preset);
    return ok;
}

// Returns the (name, value) pairs of `current` that differ from `base` (see chain.h).
std::vector<std::pair<std::string, float>> param_overrides(const std::vector<ShaderParam> &current,
                                                           const std::vector<ShaderParam> &base)
{
    std::vector<std::pair<std::string, float>> out;
    for (const ShaderParam &p : current)
    {
        // Find the parameter with the same name in `base`, if any.
        const ShaderParam *b = nullptr;
        for (const ShaderParam &x : base)
            if (x.name == p.name)
                b = &x;
        if (b == nullptr || std::fabs(p.value - b->initial) > 1e-6f)
            out.emplace_back(p.name, p.value);
    }
    return out;
}

// Loads `preset_path`, reads its parameters and compiles it for `device`. Returns false
// and sets `error` on failure, leaving the chain empty.
bool ShaderChain::create(ID3D11Device *device, const std::string &preset_path, std::string &error)
{
    destroy();
    if (!libra::loaded())
    {
        error = "librashader is not loaded";
        return false;
    }
    const libra_instance_t &api = libra::api();

    // Parse the preset file and the files it references.
    libra_shader_preset_t preset = nullptr;
    if (libra_error_t err = api.preset_create(preset_path.c_str(), &preset))
    {
        error = libra::take_error(err);
        return false;
    }

    // Read the parameters now: compiling below uses up the preset object.
    if (!read_params(preset, params_, error))
    {
        api.preset_free(&preset);
        return false;
    }

    // Compile every pass for `device`. The call takes ownership of `preset` and sets it
    // to null, so it is freed here only if librashader did not take it.
    filter_chain_d3d11_opt_t opt = {};
    opt.version = LIBRASHADER_CURRENT_VERSION;
    libra_d3d11_filter_chain_t chain = nullptr;
    // Consumes (and nulls) `preset`.
    libra_error_t err = api.d3d11_filter_chain_create(&preset, device, &opt, &chain);
    if (preset != nullptr)
        api.preset_free(&preset);
    if (err != nullptr)
    {
        error = libra::take_error(err);
        params_.clear();
        return false;
    }
    chain_ = chain;
    preset_ = preset_path;
    return true;
}

// Frees the compiled preset, if any, and clears the parameters and path.
void ShaderChain::destroy()
{
    if (chain_ != nullptr)
    {
        libra_d3d11_filter_chain_t chain = static_cast<libra_d3d11_filter_chain_t>(chain_);
        libra::api().d3d11_filter_chain_free(&chain);
        chain_ = nullptr;
    }
    params_.clear();
    preset_.clear();
}

// Runs the compiled preset once from `input` into the area (x, y, w, h) of `output`.
// Returns false and sets `error` if nothing is loaded or librashader fails.
bool ShaderChain::frame(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *input, ID3D11RenderTargetView *output,
                        int x, int y, int w, int h, uint64_t frame_count, std::string &error)
{
    if (chain_ == nullptr)
    {
        error = "no shader loaded";
        return false;
    }
    // The area of `output` to draw into.
    libra_viewport_t viewport = {float(x), float(y), uint32_t(w), uint32_t(h)};
    // Per-frame values librashader passes to the shaders: playing forwards
    // (frame_direction 1; -1 would mean rewinding), one shader run per displayed frame
    // (subframe 1 of 1), a source frame rate of 60 per second (assumed; the real rate is
    // not known here), and HDR brightness left at its default.
    frame_d3d11_opt_t opt = {};
    opt.version = LIBRASHADER_CURRENT_VERSION;
    opt.frame_direction = 1;
    opt.total_subframes = 1;
    opt.current_subframe = 1;
    opt.frames_per_second = 60.0f;
    opt.brightness_nits = 200.0f; // documented default; only HDR presets read it

    // No transform matrix is passed (nullptr), so librashader uses its default one.
    libra_d3d11_filter_chain_t chain = static_cast<libra_d3d11_filter_chain_t>(chain_);
    if (libra_error_t err = libra::api().d3d11_filter_chain_frame(&chain, ctx, size_t(frame_count), input, output,
                                                                  &viewport, nullptr, &opt))
    {
        error = libra::take_error(err);
        return false;
    }
    return true;
}

// Changes parameter `name` to `value` in the compiled preset and in params_. Returns
// false (without setting `error`) when nothing is loaded, or false with `error` set
// when librashader rejects the change.
bool ShaderChain::set_param(const std::string &name, float value, std::string &error)
{
    if (chain_ == nullptr)
        return false;
    libra_d3d11_filter_chain_t chain = static_cast<libra_d3d11_filter_chain_t>(chain_);
    if (libra_error_t err = libra::api().d3d11_filter_chain_set_param(&chain, name.c_str(), value))
    {
        error = libra::take_error(err);
        return false;
    }
    for (ShaderParam &p : params_)
        if (p.name == name)
            p.value = value;
    return true;
}
