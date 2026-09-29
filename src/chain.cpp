#include "chain.h"
#include "librashader_api.h"

bool ShaderChain::create(ID3D11Device *device, const std::string &preset_path, std::string &error)
{
    destroy();
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

    // Parameters, with the values this preset sets (it may override defaults).
    libra_preset_param_list_t list = {};
    if (api.preset_get_runtime_params(&preset, &list) == nullptr)
    {
        for (uint64_t i = 0; i < list.length; ++i)
        {
            const libra_preset_param_t &p = list.parameters[i];
            ShaderParam sp;
            sp.name = p.name ? p.name : "";
            sp.description = p.description ? p.description : sp.name;
            sp.initial = p.initial;
            sp.minimum = p.minimum;
            sp.maximum = p.maximum;
            sp.step = p.step;
            sp.value = p.initial;
            float v = 0;
            if (api.preset_get_param(&preset, sp.name.c_str(), &v) == nullptr)
                sp.value = v;
            params_.push_back(sp);
        }
        api.preset_free_runtime_params(list);
    }

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

bool ShaderChain::frame(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *input, ID3D11RenderTargetView *output,
                        int x, int y, int w, int h, uint64_t frame_count, std::string &error)
{
    if (chain_ == nullptr)
    {
        error = "no shader loaded";
        return false;
    }
    libra_viewport_t viewport = {float(x), float(y), uint32_t(w), uint32_t(h)};
    frame_d3d11_opt_t opt = {};
    opt.version = LIBRASHADER_CURRENT_VERSION;
    opt.frame_direction = 1;
    opt.total_subframes = 1;
    opt.current_subframe = 1;
    opt.frames_per_second = 60.0f;
    opt.brightness_nits = 200.0f; // documented default; only HDR presets read it

    libra_d3d11_filter_chain_t chain = static_cast<libra_d3d11_filter_chain_t>(chain_);
    if (libra_error_t err = libra::api().d3d11_filter_chain_frame(&chain, ctx, size_t(frame_count), input, output,
                                                                  &viewport, nullptr, &opt))
    {
        error = libra::take_error(err);
        return false;
    }
    return true;
}

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
