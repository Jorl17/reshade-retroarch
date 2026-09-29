#include "renderer.h"

#include "capture_ps.h"
#include "capture_vs.h"

namespace
{
template <typename T>
void release(T *&p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

// Concrete, non-sRGB format to view a resource with. Shaders must see the encoded
// values the game wrote, exactly as RetroArch hands a core's output to a preset.
DXGI_FORMAT unorm_view(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return f;
    }
}

// Typeless member of the format's family. Textures that are viewed with a format
// other than their own (an sRGB frame viewed as UNORM) must be created typeless;
// copies between a typed and a typeless texture of the same family are allowed.
DXGI_FORMAT typeless(DXGI_FORMAT f)
{
    switch (unorm_view(f))
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default:
        return f;
    }
}

struct CaptureParams
{
    uint32_t rect[4];
    uint32_t native[2];
    uint32_t padding[2];
};
}

bool Renderer::supported_format(DXGI_FORMAT f)
{
    switch (unorm_view(f))
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

bool Renderer::init(ID3D11Device *device, std::string &error)
{
    shutdown();
    device_ = device;
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(CaptureParams);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateVertexShader(g_capture_vs, sizeof(g_capture_vs), nullptr, &vs_)) ||
        FAILED(device->CreatePixelShader(g_capture_ps, sizeof(g_capture_ps), nullptr, &ps_)) ||
        FAILED(device->CreateBuffer(&cbd, nullptr, &cb_)))
    {
        error = "could not create the capture shader";
        shutdown();
        return false;
    }
    return true;
}

void Renderer::shutdown()
{
    release(out_rtv_);
    release(out_tex_);
    out_desc_ = {};
    out_format_ = DXGI_FORMAT_UNKNOWN;
    release(native_srv_);
    release(native_rtv_);
    release(native_tex_);
    native_w_ = native_h_ = 0;
    release(snap_srv_);
    release(snap_tex_);
    snap_desc_ = {};
    snap_format_ = DXGI_FORMAT_UNKNOWN;
    release(cb_);
    release(ps_);
    release(vs_);
    device_ = nullptr;
}

ID3D11Texture2D *Renderer::snapshot(ID3D11DeviceContext *ctx, ID3D11Texture2D *frame, std::string &error)
{
    D3D11_TEXTURE2D_DESC fd;
    frame->GetDesc(&fd);
    if (!supported_format(fd.Format))
    {
        error = "unsupported back buffer format " + std::to_string(int(fd.Format)) + " (HDR is not supported yet)";
        return nullptr;
    }

    if (snap_tex_ == nullptr || fd.Width != snap_desc_.Width || fd.Height != snap_desc_.Height ||
        fd.Format != snap_format_)
    {
        release(snap_srv_);
        release(snap_tex_);
        snap_desc_ = {};
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = fd.Width;
        d.Height = fd.Height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = typeless(fd.Format);
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
        vd.Format = unorm_view(fd.Format);
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &snap_tex_)) ||
            FAILED(device_->CreateShaderResourceView(snap_tex_, &vd, &snap_srv_)))
        {
            release(snap_tex_);
            error = "could not create the frame snapshot texture";
            return nullptr;
        }
        snap_desc_ = d;
        snap_format_ = fd.Format;
    }

    // A typed source must be resolved with its own format; a typeless one with the view format.
    if (fd.SampleDesc.Count > 1)
        ctx->ResolveSubresource(snap_tex_, 0, frame, 0, typeless(fd.Format) == fd.Format ? unorm_view(fd.Format) : fd.Format);
    else
        ctx->CopyResource(snap_tex_, frame);
    return snap_tex_;
}

bool Renderer::ensure_native(int w, int h, std::string &error)
{
    if (native_tex_ != nullptr && w == native_w_ && h == native_h_)
        return true;
    release(native_srv_);
    release(native_rtv_);
    release(native_tex_);
    native_w_ = native_h_ = 0;

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = UINT(w);
    d.Height = UINT(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&d, nullptr, &native_tex_)) ||
        FAILED(device_->CreateRenderTargetView(native_tex_, nullptr, &native_rtv_)) ||
        FAILED(device_->CreateShaderResourceView(native_tex_, nullptr, &native_srv_)))
    {
        release(native_srv_);
        release(native_rtv_);
        release(native_tex_);
        error = "could not create the native image texture";
        return false;
    }
    native_w_ = w;
    native_h_ = h;
    return true;
}

bool Renderer::ensure_output(int w, int h, DXGI_FORMAT format, std::string &error)
{
    if (out_tex_ != nullptr && UINT(w) == out_desc_.Width && UINT(h) == out_desc_.Height && format == out_format_)
        return true;
    release(out_rtv_);
    release(out_tex_);
    out_desc_ = {};

    // Same format family as the frame, so it can be copied into it.
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = UINT(w);
    d.Height = UINT(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = typeless(format);
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = unorm_view(format);
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device_->CreateTexture2D(&d, nullptr, &out_tex_)) ||
        FAILED(device_->CreateRenderTargetView(out_tex_, &rd, &out_rtv_)))
    {
        release(out_rtv_);
        release(out_tex_);
        error = "could not create the output texture";
        return false;
    }
    out_desc_ = d;
    out_format_ = format;
    return true;
}

bool Renderer::render(ID3D11DeviceContext *ctx, const PixelGrid &grid, ShaderChain &chain, ID3D11Texture2D *dst,
                      uint64_t frame_count, std::string &error)
{
    if (snap_tex_ == nullptr || !grid.valid)
    {
        error = "nothing to render";
        return false;
    }
    if (grid.rect_x < 0 || grid.rect_y < 0 || UINT(grid.rect_x + grid.rect_w) > snap_desc_.Width ||
        UINT(grid.rect_y + grid.rect_h) > snap_desc_.Height)
    {
        error = "pixel grid does not fit the frame";
        return false;
    }
    if (!ensure_native(grid.native_w, grid.native_h, error))
        return false;

    const CaptureParams params = {
        {uint32_t(grid.rect_x), uint32_t(grid.rect_y), uint32_t(grid.rect_w), uint32_t(grid.rect_h)},
        {uint32_t(grid.native_w), uint32_t(grid.native_h)},
        {0, 0}};
    ctx->UpdateSubresource(cb_, 0, nullptr, &params, 0, 0);

    ID3D11ShaderResourceView *null_srv = nullptr;
    ID3D11RenderTargetView *null_rtv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(1, &native_rtv_, nullptr);
    const D3D11_VIEWPORT vp = {0.0f, 0.0f, float(grid.native_w), float(grid.native_h), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(ps_, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb_);
    ctx->PSSetShaderResources(0, 1, &snap_srv_);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);

    D3D11_TEXTURE2D_DESC dd;
    dst->GetDesc(&dd);
    if (dd.SampleDesc.Count != 1)
    {
        error = "multisampled output is not supported";
        return false;
    }
    if (!ensure_output(grid.rect_w, grid.rect_h, dd.Format, error) ||
        !chain.frame(ctx, native_srv_, out_rtv_, 0, 0, grid.rect_w, grid.rect_h, frame_count, error))
        return false;
    const D3D11_BOX box = {0, 0, 0, UINT(grid.rect_w), UINT(grid.rect_h), 1};
    ctx->CopySubresourceRegion(dst, 0, UINT(grid.rect_x), UINT(grid.rect_y), 0, out_tex_, 0, &box);
    return true;
}
