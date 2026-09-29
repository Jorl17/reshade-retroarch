#include "backend.h"

#include <d3d9.h>

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

// Direct3D 9, windowed. Back buffers are X8R8G8B8 (BGRA byte order); D3D9 has no
// windowed 10-bit or sRGB back buffer formats.
class D3D9 : public Backend
{
public:
    ~D3D9() override
    {
        drop_pictures();
        release(dev_);
        release(d3d_);
    }

    bool init(const Options &o, std::string &error, bool &unsupported) override
    {
        o_ = o;
        if (o.format != Format::rgba8 || o.hdr10)
        {
            error = "Direct3D 9 windowed back buffers are 8-bit SDR only";
            unsupported = true;
            return false;
        }
        d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
        if (d3d_ == nullptr)
        {
            error = "Direct3DCreate9 failed";
            return false;
        }
        pp_.Windowed = TRUE;
        pp_.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp_.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp_.BackBufferWidth = o.width;
        pp_.BackBufferHeight = o.height;
        pp_.BackBufferCount = 1;
        pp_.hDeviceWindow = o.hwnd;
        pp_.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        const HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, o.hwnd,
                                              D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp_, &dev_);
        if (FAILED(hr))
        {
            error = "CreateDevice failed " + std::to_string(hr);
            return false;
        }
        return true;
    }

    bool resize(UINT w, UINT h, std::string &error) override
    {
        drop_pictures(); // default-pool resources must go before Reset
        pp_.BackBufferWidth = w;
        pp_.BackBufferHeight = h;
        if (FAILED(dev_->Reset(&pp_)))
        {
            error = "Reset failed";
            return false;
        }
        return true;
    }

    bool draw(const Image *img, std::string &error) override
    {
        if (img == nullptr)
        {
            if (FAILED(dev_->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0)))
            {
                error = "Clear failed";
                return false;
            }
            return true;
        }
        IDirect3DSurface9 *pic = picture(*img), *bb = nullptr;
        if (pic == nullptr || FAILED(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        {
            error = "could not create the picture surface";
            return false;
        }
        const bool ok = SUCCEEDED(dev_->StretchRect(pic, nullptr, bb, nullptr, D3DTEXF_NONE)); // same size: a copy
        bb->Release();
        if (!ok)
            error = "StretchRect failed";
        return ok;
    }

    bool read_back(Image &out, std::string &error) override
    {
        IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
        D3DLOCKED_RECT lr = {};
        bool ok = SUCCEEDED(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) &&
                  SUCCEEDED(dev_->CreateOffscreenPlainSurface(pp_.BackBufferWidth, pp_.BackBufferHeight, pp_.BackBufferFormat,
                                                              D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
                  SUCCEEDED(dev_->GetRenderTargetData(bb, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        if (ok)
        {
            decode(static_cast<const uint8_t *>(lr.pBits), size_t(lr.Pitch), pp_.BackBufferWidth, pp_.BackBufferHeight,
                   Format::rgba8, true, out);
            sys->UnlockRect();
        }
        else
            error = "could not read the back buffer";
        release(sys);
        release(bb);
        return ok;
    }

    bool present(std::string &error) override
    {
        if (FAILED(dev_->Present(nullptr, nullptr, nullptr, nullptr)))
        {
            error = "Present failed";
            return false;
        }
        return true;
    }

private:
    // The picture in a default-pool surface (StretchRect's source must be in video memory).
    IDirect3DSurface9 *picture(const Image &img)
    {
        if (auto it = pictures_.find(&img); it != pictures_.end())
            return it->second;
        IDirect3DSurface9 *sys = nullptr, *vid = nullptr;
        D3DLOCKED_RECT lr = {};
        bool ok = SUCCEEDED(dev_->CreateOffscreenPlainSurface(img.w, img.h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
                  SUCCEEDED(sys->LockRect(&lr, nullptr, 0));
        if (ok)
        {
            const std::vector<uint8_t> bytes = encode(img, Format::rgba8, true);
            for (UINT y = 0; y < img.h; ++y)
                std::memcpy(static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch, &bytes[size_t(y) * img.w * 4], img.w * 4);
            sys->UnlockRect();
            ok = SUCCEEDED(dev_->CreateOffscreenPlainSurface(img.w, img.h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &vid, nullptr)) &&
                 SUCCEEDED(dev_->UpdateSurface(sys, nullptr, vid, nullptr));
        }
        release(sys);
        if (!ok)
        {
            release(vid);
            return nullptr;
        }
        return pictures_[&img] = vid;
    }

    void drop_pictures()
    {
        for (auto &[img, s] : pictures_)
            s->Release();
        pictures_.clear();
    }

    Options o_;
    IDirect3D9 *d3d_ = nullptr;
    IDirect3DDevice9 *dev_ = nullptr;
    D3DPRESENT_PARAMETERS pp_ = {};
    std::map<const Image *, IDirect3DSurface9 *> pictures_;
};
}

std::unique_ptr<Backend> make_d3d9()
{
    return std::make_unique<D3D9>();
}
