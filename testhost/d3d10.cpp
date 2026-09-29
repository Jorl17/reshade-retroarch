#include "backend.h"

#include <d3d10.h>

#include <map>

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

DXGI_FORMAT dxgi_format(Format f)
{
    return f == Format::rgb10a2     ? DXGI_FORMAT_R10G10B10A2_UNORM
           : f == Format::rgba8srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                    : DXGI_FORMAT_R8G8B8A8_UNORM;
}

// Direct3D 10 with the classic (bitblt) swap chain, as D3D10 games use.
class D3D10 : public Backend
{
public:
    ~D3D10() override
    {
        for (auto &[img, tex] : textures_)
            tex->Release();
        if (dev_)
            dev_->ClearState();
        release(dev_);
        release(swap_);
    }

    bool init(const Options &o, std::string &error, bool &unsupported) override
    {
        o_ = o;
        if (o.hdr10)
        {
            error = "HDR10 needs a flip-model swap chain, which Direct3D 10 games do not use";
            unsupported = true;
            return false;
        }
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = o.width;
        sd.BufferDesc.Height = o.height;
        sd.BufferDesc.RefreshRate = {60, 1};
        sd.BufferDesc.Format = dxgi_format(o.format);
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = o.hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const HRESULT hr = D3D10CreateDeviceAndSwapChain(nullptr, o.warp ? D3D10_DRIVER_TYPE_WARP : D3D10_DRIVER_TYPE_HARDWARE,
                                                         nullptr, 0, D3D10_SDK_VERSION, &sd, &swap_, &dev_);
        if (FAILED(hr))
        {
            error = "D3D10CreateDeviceAndSwapChain failed " + std::to_string(hr);
            return false;
        }
        return true;
    }

    bool resize(UINT w, UINT h, std::string &error) override
    {
        dev_->ClearState();
        if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
        {
            error = "ResizeBuffers failed";
            return false;
        }
        return true;
    }

    bool draw(const Image *img, std::string &error) override
    {
        ID3D10Texture2D *bb = nullptr;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb))))
        {
            error = "GetBuffer failed";
            return false;
        }
        bool ok = true;
        if (img != nullptr)
        {
            ID3D10Texture2D *tex = texture(*img);
            ok = tex != nullptr;
            if (ok)
                dev_->CopyResource(bb, tex);
            else
                error = "could not create the picture texture";
        }
        else
        {
            ID3D10RenderTargetView *rtv = nullptr;
            ok = SUCCEEDED(dev_->CreateRenderTargetView(bb, nullptr, &rtv));
            if (ok)
            {
                const float black[4] = {0, 0, 0, 1};
                dev_->ClearRenderTargetView(rtv, black);
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
        ID3D10Texture2D *bb = nullptr;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb))))
        {
            error = "GetBuffer failed";
            return false;
        }
        D3D10_TEXTURE2D_DESC d;
        bb->GetDesc(&d);
        d.Usage = D3D10_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ID3D10Texture2D *staging = nullptr;
        D3D10_MAPPED_TEXTURE2D m = {};
        bool ok = SUCCEEDED(dev_->CreateTexture2D(&d, nullptr, &staging));
        if (ok)
        {
            dev_->CopyResource(staging, bb);
            ok = SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &m));
        }
        if (ok)
        {
            decode(static_cast<const uint8_t *>(m.pData), m.RowPitch, d.Width, d.Height, o_.format, false, out);
            staging->Unmap(0);
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
    ID3D10Texture2D *texture(const Image &img)
    {
        if (auto it = textures_.find(&img); it != textures_.end())
            return it->second;
        D3D10_TEXTURE2D_DESC td = {};
        td.Width = img.w;
        td.Height = img.h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxgi_format(o_.format);
        td.SampleDesc.Count = 1;
        td.Usage = D3D10_USAGE_IMMUTABLE;
        td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
        const std::vector<uint8_t> bytes = encode(img, o_.format, false);
        const D3D10_SUBRESOURCE_DATA init = {bytes.data(), img.w * 4, 0};
        ID3D10Texture2D *tex = nullptr;
        if (FAILED(dev_->CreateTexture2D(&td, &init, &tex)))
            return nullptr;
        return textures_[&img] = tex;
    }

    Options o_;
    IDXGISwapChain *swap_ = nullptr;
    ID3D10Device *dev_ = nullptr;
    std::map<const Image *, ID3D10Texture2D *> textures_;
};
}

std::unique_ptr<Backend> make_d3d10()
{
    return std::make_unique<D3D10>();
}
