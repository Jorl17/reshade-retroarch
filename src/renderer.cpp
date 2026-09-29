// Implementation of renderer.h: copies the game's frame, rebuilds the native
// (original, low-resolution) picture from it with the capture preset, runs the
// RetroArch preset on that picture and copies the result back into the frame.
// Everything here is Direct3D 11.

#include "renderer.h"

#include "capture.h"

namespace
{
// Calls Release() on the COM object `p` points to (dropping this code's reference to
// it) and sets `p` to null. Does nothing if `p` is already null.
template <typename T>
void release(T *&p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

// Background for the two functions below. A Direct3D format fixes how many bits each
// channel of a pixel has and how shaders interpret them. "UNORM" formats hand the
// stored integers to shaders as 0..1 with no conversion; "UNORM_SRGB" formats decode
// them from sRGB to linear light first. Formats with the same bit layout form a family
// (R8G8B8A8_UNORM, R8G8B8A8_UNORM_SRGB, ...), and its "TYPELESS" member fixes only the
// layout: each view of such a texture chooses the interpretation.

// Returns the plain UNORM format to read or draw a texture of format `f` with: the sRGB
// and typeless members of the supported 8-bit and 10-bit families map to their UNORM
// member, and every other format is returned unchanged. Shaders must see the values the
// game stored, not sRGB-decoded ones, exactly as RetroArch hands a core's output (a
// core is RetroArch's name for an emulator) to a preset.
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

// Returns the TYPELESS member of the family `f` belongs to (for example
// R8G8B8A8_TYPELESS for R8G8B8A8_UNORM_SRGB), or `f` unchanged for formats not handled
// here. A texture must be created typeless to be viewed with a format other than its
// own (the copy of an sRGB frame is read as UNORM). Copies between a typed and a
// typeless texture of the same family are allowed, so such a texture can still be
// filled from, or copied into, the game's frame.
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

}

// Returns true for the frame formats the capture can read (see renderer.h).
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

// Loads the capture preset for `device`. Returns false and sets `error` if it cannot be
// loaded, leaving the renderer empty.
bool Renderer::init(ID3D11Device *device, std::string &error)
{
    shutdown();
    device_ = device;
    const std::string preset = capture_preset_path(error);
    if (preset.empty() || !capture_.create(device, preset, error))
    {
        error = "could not load the capture shader: " + error;
        shutdown();
        return false;
    }
    return true;
}

// Releases every GPU object and forgets the device and all sizes and formats.
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
    capture_.destroy();
    capture_grid_ = PixelGrid();
    device_ = nullptr;
}

// Copies `frame` into snap_tex_ and returns snap_tex_, or returns nullptr and sets
// `error` if the frame's format is not supported or the copy cannot be created.
ID3D11Texture2D *Renderer::snapshot(ID3D11DeviceContext *ctx, ID3D11Texture2D *frame, std::string &error)
{
    D3D11_TEXTURE2D_DESC fd;
    frame->GetDesc(&fd);
    if (!supported_format(fd.Format))
    {
        error = "unsupported back buffer format " + std::to_string(int(fd.Format)) + " (HDR is not supported yet)";
        return nullptr;
    }

    // (Re)create the copy when there is none yet or the frame's size or format changed
    // (window resize, resolution change). It is single-sample, typeless in the frame's
    // family, and read by shaders as plain UNORM.
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

    // Copy the frame. A multisampled frame (MSAA: several colour samples per pixel, to
    // smooth edges) cannot be copied into a single-sample texture, so its samples are
    // averaged into one per pixel with ResolveSubresource instead. That call needs a
    // concrete format: a frame created with one is resolved in its own format; a
    // typeless frame is resolved as plain UNORM.
    if (fd.SampleDesc.Count > 1)
        ctx->ResolveSubresource(snap_tex_, 0, frame, 0, typeless(fd.Format) == fd.Format ? unorm_view(fd.Format) : fd.Format);
    else
        ctx->CopyResource(snap_tex_, frame);
    return snap_tex_;
}

// Makes sure native_tex_ is `w` x `h`: keeps it if it already is, otherwise creates a
// new RGBA 8-bit texture with a render target view (for the capture shader to draw
// into) and a shader resource view (for the shader chain to read).
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

// Makes sure out_tex_ is `w` x `h` and was made for frame format `format`: keeps it if
// so, otherwise creates a new one with a render target view for librashader to draw
// into.
bool Renderer::ensure_output(int w, int h, DXGI_FORMAT format, std::string &error)
{
    if (out_tex_ != nullptr && UINT(w) == out_desc_.Width && UINT(h) == out_desc_.Height && format == out_format_)
        return true;
    release(out_rtv_);
    release(out_tex_);
    out_desc_ = {};

    // Typeless in the frame's format family, so it can be copied into the frame, and
    // drawn into as plain UNORM, so shaders write the values that end up in the frame.
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

// Rebuilds the native image from the last snapshot, runs `chain` on it and copies the
// result into `dst` over grid's rectangle (see renderer.h). Returns false and sets
// `error` on failure; `dst` is then unchanged.
bool Renderer::render(ID3D11DeviceContext *ctx, const PixelGrid &grid, ShaderChain &chain, ID3D11Texture2D *dst,
                      uint64_t frame_count, std::string &error)
{
    // Refuse to run without a snapshot or a valid grid, or with a grid whose rectangle
    // reaches outside the snapshot.
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

    // Step 1: rebuild the native image: run the capture preset on the snapshot, into
    // native_tex_ (native_w x native_h). Its one pass writes each native pixel from the
    // frame pixel at the centre of that pixel's block (capture.slang).
    if (!set_capture_grid(grid, error) ||
        !capture_.frame(ctx, snap_srv_, native_rtv_, 0, 0, grid.native_w, grid.native_h, frame_count, error))
        return false;

    // The final copy needs `dst` to have one sample per pixel, like out_tex_.
    D3D11_TEXTURE2D_DESC dd;
    dst->GetDesc(&dd);
    if (dd.SampleDesc.Count != 1)
    {
        error = "multisampled output is not supported";
        return false;
    }
    // Step 2: run the preset on the native image, into out_tex_, which is exactly the
    // size of the rectangle. Step 3: copy out_tex_ into `dst` at the rectangle's
    // position, leaving the rest of `dst` as it was.
    if (!ensure_output(grid.rect_w, grid.rect_h, dd.Format, error) ||
        !chain.frame(ctx, native_srv_, out_rtv_, 0, 0, grid.rect_w, grid.rect_h, frame_count, error))
        return false;
    const D3D11_BOX box = {0, 0, 0, UINT(grid.rect_w), UINT(grid.rect_h), 1};
    ctx->CopySubresourceRegion(dst, 0, UINT(grid.rect_x), UINT(grid.rect_y), 0, out_tex_, 0, &box);
    return true;
}

// Sets the capture preset's six parameters (capture.slang) from `grid`, skipping the
// work when the grid has not changed since the last call.
bool Renderer::set_capture_grid(const PixelGrid &grid, std::string &error)
{
    if (grid.same_as(capture_grid_))
        return true;
    const std::pair<const char *, int> values[] = {
        {"rra_rect_x", grid.rect_x},       {"rra_rect_y", grid.rect_y},       {"rra_rect_w", grid.rect_w},
        {"rra_rect_h", grid.rect_h},       {"rra_native_w", grid.native_w}, {"rra_native_h", grid.native_h}};
    for (const auto &[name, value] : values)
        if (!capture_.set_param(name, float(value), error))
            return false;
    capture_grid_ = grid;
    return true;
}
