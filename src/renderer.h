#pragma once

#include "chain.h"
#include "grid_detect.h"

#include <d3d11.h>
#include <cstdint>
#include <string>

// Takes the game's frame, recovers its native image and runs a shader chain on it.
// Used by the ReShade add-on and by the offline tools, so both run the same code.
class Renderer
{
public:
    Renderer() = default;
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    ~Renderer() { shutdown(); }

    bool init(ID3D11Device *device, std::string &error);
    void shutdown();

    // Copies the game's frame into a single-sample texture the capture pass can
    // read (and the grid detector can read back). Must run before anything draws
    // over the frame. Returns nullptr and sets `error` for unsupported formats.
    ID3D11Texture2D *snapshot(ID3D11DeviceContext *ctx, ID3D11Texture2D *frame, std::string &error);

    // Recovers the native image described by `grid` from the last snapshot, runs
    // `chain` on it, and writes the result into `dst` over grid's rectangle. Pixels
    // outside the rectangle (letterbox bars, border art) are never touched:
    // librashader clears its whole output target, so it renders into a texture of
    // the rectangle's size which is then copied into place.
    bool render(ID3D11DeviceContext *ctx, const PixelGrid &grid, ShaderChain &chain, ID3D11Texture2D *dst,
                uint64_t frame_count, std::string &error);

    ID3D11Texture2D *native_texture() const { return native_tex_; }
    ID3D11Texture2D *snapshot_texture() const { return snap_tex_; }

    // Formats the capture understands: 8-bit and 10-bit SDR. HDR (float) frames
    // hold linear or scRGB values that RetroArch shaders are not written for.
    static bool supported_format(DXGI_FORMAT f);

private:
    bool ensure_native(int w, int h, std::string &error);
    bool ensure_output(int w, int h, DXGI_FORMAT format, std::string &error);

    ID3D11Device *device_ = nullptr;
    ID3D11VertexShader *vs_ = nullptr;
    ID3D11PixelShader *ps_ = nullptr;
    ID3D11Buffer *cb_ = nullptr;

    ID3D11Texture2D *snap_tex_ = nullptr;
    ID3D11ShaderResourceView *snap_srv_ = nullptr;
    D3D11_TEXTURE2D_DESC snap_desc_ = {};

    ID3D11Texture2D *native_tex_ = nullptr;
    ID3D11RenderTargetView *native_rtv_ = nullptr;
    ID3D11ShaderResourceView *native_srv_ = nullptr;
    int native_w_ = 0, native_h_ = 0;

    ID3D11Texture2D *out_tex_ = nullptr;
    ID3D11RenderTargetView *out_rtv_ = nullptr;
    D3D11_TEXTURE2D_DESC out_desc_ = {};
};
