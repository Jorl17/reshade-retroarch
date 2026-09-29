#pragma once

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

struct ShaderParam
{
    std::string name, description;
    float value = 0, initial = 0, minimum = 0, maximum = 0, step = 0;
};

// A RetroArch slang preset compiled for a D3D11 device by librashader.
class ShaderChain
{
public:
    ShaderChain() = default;
    ShaderChain(const ShaderChain &) = delete;
    ShaderChain &operator=(const ShaderChain &) = delete;
    ~ShaderChain() { destroy(); }

    // Compiles `preset_path` (UTF-8). Creates D3D11 resources on the calling thread.
    bool create(ID3D11Device *device, const std::string &preset_path, std::string &error);
    void destroy();
    bool ready() const { return chain_ != nullptr; }

    // Runs the preset on `input` (the native image) into `output`, over the
    // rectangle (x, y, w, h). Pixels outside the rectangle are left untouched.
    bool frame(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *input, ID3D11RenderTargetView *output, int x,
               int y, int w, int h, uint64_t frame_count, std::string &error);

    bool set_param(const std::string &name, float value, std::string &error);
    const std::vector<ShaderParam> &params() const { return params_; }
    const std::string &preset() const { return preset_; }

private:
    void *chain_ = nullptr; // libra_d3d11_filter_chain_t
    std::vector<ShaderParam> params_;
    std::string preset_;
};
