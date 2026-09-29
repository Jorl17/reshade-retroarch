// Direct3D 11 backend of the test host (see backend.h): a DXGI swap chain on a D3D11
// device; pictures are uploaded once as textures and copied into the back buffer.
#include "backend.h"

#include <d3d11.h>
#include <dxgi1_4.h>

#include <map>

namespace
{
// Releases COM object `p` (if any) and sets the pointer to null.
template <typename T>
void release(T *&p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

// The DXGI format used for back buffers and pictures of format `f`.
DXGI_FORMAT dxgi_format(Format f)
{
    return f == Format::rgb10a2     ? DXGI_FORMAT_R10G10B10A2_UNORM
           : f == Format::rgba8srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                    : DXGI_FORMAT_R8G8B8A8_UNORM;
}

// See Backend in backend.h for what each function does.
class D3D11 : public Backend
{
public:
    ~D3D11() override
    {
        for (auto &[img, tex] : textures_)
            tex->Release();
        if (ctx_)
            ctx_->ClearState();
        release(ctx_);
        release(dev_);
        release(swap_);
    }

    bool init(const Options &o, std::string &error, bool &unsupported) override
    {
        o_ = o;
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = o.width;
        sd.BufferDesc.Height = o.height;
        sd.BufferDesc.RefreshRate = {60, 1};
        sd.BufferDesc.Format = dxgi_format(o.format);
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = o.hwnd;
        sd.Windowed = TRUE;
        // Flip-model swap chains cannot be sRGB, so sRGB uses the older bitblt model.
        const bool bitblt = o.format == Format::rgba8srgb;
        sd.BufferCount = bitblt ? 1 : 2;
        sd.SwapEffect = bitblt ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, o.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                                         nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap_, &dev_,
                                                         nullptr, &ctx_);
        if (FAILED(hr))
        {
            error = "D3D11CreateDeviceAndSwapChain failed " + std::to_string(hr);
            return false;
        }
        if (o.hdr10)
        {
            IDXGISwapChain3 *swap3 = nullptr;
            HRESULT ch = swap_->QueryInterface(IID_PPV_ARGS(&swap3));
            if (SUCCEEDED(ch))
            {
                ch = swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                swap3->Release();
            }
            if (FAILED(ch))
            {
                error = "HDR10 colour space not available";
                unsupported = true;
                return false;
            }
        }
        return true;
    }

    bool resize(UINT w, UINT h, std::string &error) override
    {
        ctx_->ClearState();
        ctx_->Flush();
        if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
        {
            error = "ResizeBuffers failed";
            return false;
        }
        return true;
    }

    bool draw(const Image *img, std::string &error) override
    {
        ID3D11Texture2D *bb = nullptr;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb))))
        {
            error = "GetBuffer failed";
            return false;
        }
        bool ok = true;
        if (img != nullptr)
        {
            // Same size and format, so CopyResource copies the bytes unchanged.
            ID3D11Texture2D *tex = texture(*img);
            ok = tex != nullptr;
            if (ok)
                ctx_->CopyResource(bb, tex);
            else
                error = "could not create the picture texture";
        }
        else
        {
            ID3D11RenderTargetView *rtv = nullptr;
            ok = SUCCEEDED(dev_->CreateRenderTargetView(bb, nullptr, &rtv));
            if (ok)
            {
                const float black[4] = {0, 0, 0, 1};
                ctx_->ClearRenderTargetView(rtv, black);
                rtv->Release();
            }
            else
                error = "CreateRenderTargetView failed";
        }
        bb->Release();
        return ok;
    }

    bool read_back(Image &out, std::string &error) override
    {
        ID3D11Texture2D *bb = nullptr;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb))))
        {
            error = "GetBuffer failed";
            return false;
        }
        // Copy to a CPU-readable ("staging") texture and read that.
        D3D11_TEXTURE2D_DESC d;
        bb->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ID3D11Texture2D *staging = nullptr;
        D3D11_MAPPED_SUBRESOURCE m = {};
        bool ok = SUCCEEDED(dev_->CreateTexture2D(&d, nullptr, &staging));
        if (ok)
        {
            ctx_->CopyResource(staging, bb);
            ok = SUCCEEDED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &m));
        }
        if (ok)
        {
            decode(static_cast<const uint8_t *>(m.pData), m.RowPitch, d.Width, d.Height, o_.format, false, out);
            ctx_->Unmap(staging, 0);
        }
        else
            error = "could not read the back buffer";
        release(staging);
        bb->Release();
        return ok;
    }

    bool present(std::string &error) override
    {
        if (FAILED(swap_->Present(0, 0)))
        {
            error = "Present failed";
            return false;
        }
        return true;
    }

private:
    // Returns a texture holding `img` in the back buffer's format, created on first use
    // and kept for the next frames (so draw is a plain GPU copy). Null on failure.
    ID3D11Texture2D *texture(const Image &img)
    {
        if (auto it = textures_.find(&img); it != textures_.end())
            return it->second;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = img.w;
        td.Height = img.h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxgi_format(o_.format); // same as the back buffer, for CopyResource
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const std::vector<uint8_t> bytes = encode(img, o_.format, false);
        const D3D11_SUBRESOURCE_DATA init = {bytes.data(), img.w * 4, 0};
        ID3D11Texture2D *tex = nullptr;
        if (FAILED(dev_->CreateTexture2D(&td, &init, &tex)))
            return nullptr;
        return textures_[&img] = tex;
    }

    Options o_;
    IDXGISwapChain *swap_ = nullptr;
    ID3D11Device *dev_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    std::map<const Image *, ID3D11Texture2D *> textures_;
};
}

std::unique_ptr<Backend> make_d3d11()
{
    return std::make_unique<D3D11>();
}
