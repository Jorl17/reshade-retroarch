// Direct3D 12 backend of the test host (see backend.h): a flip-model swap chain on a D3D12
// device; pictures are uploaded once into CPU-visible buffers and copied into the back
// buffer with CopyTextureRegion.
#include "backend.h"

#include <d3d12.h>
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

constexpr UINT kBuffers = 2; // back buffers in the swap chain

// The DXGI format used for back buffers of format `f` (Direct3D 12 has no sRGB swap chains).
DXGI_FORMAT dxgi_format(Format f)
{
    return f == Format::rgb10a2 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
}

// Every operation records one command list, submits it and waits until the GPU has
// finished (see wait()): slow, but simple, and speed does not matter in a test host.
// Back buffers are kept in the PRESENT state between operations. See Backend in
// backend.h for what each public function does.
class D3D12 : public Backend
{
public:
    ~D3D12() override
    {
        if (queue_ != nullptr)
            wait();
        for (auto &[img, buf] : uploads_)
            buf->Release();
        release_buffers();
        release(rtv_heap_);
        release(list_);
        release(alloc_);
        release(fence_);
        if (event_ != nullptr)
            CloseHandle(event_);
        release(swap_);
        release(queue_);
        release(dev_);
        release(factory_);
    }

    bool init(const Options &o, std::string &error, bool &unsupported) override
    {
        o_ = o;
        if (o.format == Format::rgba8srgb)
        {
            error = "Direct3D 12 swap chains cannot be sRGB (flip model only)";
            unsupported = true;
            return false;
        }
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_))))
        {
            error = "CreateDXGIFactory2 failed";
            return false;
        }
        IDXGIAdapter *adapter = nullptr;
        if (o.warp)
            factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_));
        release(adapter);
        if (FAILED(hr))
        {
            error = "D3D12CreateDevice failed " + std::to_string(hr);
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_))) ||
            FAILED(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_))) ||
            FAILED(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_, nullptr, IID_PPV_ARGS(&list_))) ||
            FAILED(list_->Close()) || FAILED(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
        {
            error = "could not create the command queue";
            return false;
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = o.width;
        sd.Height = o.height;
        sd.Format = dxgi_format(o.format);
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1 *swap1 = nullptr;
        if (FAILED(factory_->CreateSwapChainForHwnd(queue_, o.hwnd, &sd, nullptr, nullptr, &swap1)) ||
            FAILED(swap1->QueryInterface(IID_PPV_ARGS(&swap_))))
        {
            release(swap1);
            error = "could not create the swap chain";
            return false;
        }
        release(swap1);
        if (o.hdr10 && FAILED(swap_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)))
        {
            error = "HDR10 colour space not available";
            unsupported = true;
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBuffers;
        if (FAILED(dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_))) || !get_buffers())
        {
            error = "could not create the back buffer views";
            return false;
        }
        return true;
    }

    bool resize(UINT w, UINT h, std::string &error) override
    {
        wait();
        release_buffers();
        if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)) || !get_buffers())
        {
            error = "ResizeBuffers failed";
            return false;
        }
        return true;
    }

    bool draw(const Image *img, std::string &error) override
    {
        ID3D12Resource *bb = buffers_[swap_->GetCurrentBackBufferIndex()];
        ID3D12Resource *upload = img != nullptr ? upload_buffer(*img) : nullptr;
        if (img != nullptr && upload == nullptr)
        {
            error = "could not create the picture upload buffer";
            return false;
        }
        begin();
        if (upload != nullptr)
        {
            barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = bb;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprint(img->w, img->h);
            list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        }
        else
        {
            barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float black[4] = {0, 0, 0, 1};
            list_->ClearRenderTargetView(rtv(swap_->GetCurrentBackBufferIndex()), black, 0, nullptr);
            barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
        return submit(error);
    }

    bool read_back(Image &out, std::string &error) override
    {
        ID3D12Resource *bb = buffers_[swap_->GetCurrentBackBufferIndex()];
        const D3D12_RESOURCE_DESC rd = bb->GetDesc();
        const UINT w = UINT(rd.Width), h = rd.Height;
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = footprint(w, h);
        ID3D12Resource *readback = buffer(D3D12_HEAP_TYPE_READBACK, UINT64(fp.Footprint.RowPitch) * h);
        if (readback == nullptr)
        {
            error = "could not create the readback buffer";
            return false;
        }
        begin();
        barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        bool ok = submit(error);
        void *data = nullptr;
        if (ok && SUCCEEDED(readback->Map(0, nullptr, &data)))
        {
            decode(static_cast<const uint8_t *>(data), fp.Footprint.RowPitch, w, h, o_.format, false, out);
            readback->Unmap(0, nullptr);
        }
        else if (ok)
        {
            error = "could not map the readback buffer";
            ok = false;
        }
        readback->Release();
        return ok;
    }

    bool present(std::string &error) override
    {
        if (FAILED(swap_->Present(0, 0)))
        {
            error = "Present failed";
            return false;
        }
        wait();
        return true;
    }

private:
    // Layout of a `w` x `h` picture inside a buffer, as CopyTextureRegion needs it: rows
    // padded to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256) bytes.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint(UINT w, UINT h) const
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        fp.Footprint.Format = dxgi_format(o_.format);
        fp.Footprint.Width = w;
        fp.Footprint.Height = h;
        fp.Footprint.Depth = 1;
        fp.Footprint.RowPitch = (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        return fp;
    }

    // Creates a buffer of `size` bytes: UPLOAD heap (CPU writes, GPU reads) or READBACK
    // heap (GPU writes, CPU reads). Null on failure.
    ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = type;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *r = nullptr;
        const D3D12_RESOURCE_STATES state =
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
        return SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))) ? r
                                                                                                                          : nullptr;
    }

    // Returns an upload buffer holding `img` in the back buffer's format and row layout
    // (see footprint), created on first use and kept for the next frames. Null on failure.
    ID3D12Resource *upload_buffer(const Image &img)
    {
        if (auto it = uploads_.find(&img); it != uploads_.end())
            return it->second;
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = footprint(img.w, img.h);
        ID3D12Resource *up = buffer(D3D12_HEAP_TYPE_UPLOAD, UINT64(fp.Footprint.RowPitch) * img.h);
        void *data = nullptr;
        if (up == nullptr || FAILED(up->Map(0, nullptr, &data)))
        {
            release(up);
            return nullptr;
        }
        const std::vector<uint8_t> bytes = encode(img, o_.format, false);
        for (UINT y = 0; y < img.h; ++y)
            std::memcpy(static_cast<uint8_t *>(data) + size_t(y) * fp.Footprint.RowPitch, &bytes[size_t(y) * img.w * 4],
                        img.w * 4);
        up->Unmap(0, nullptr);
        return uploads_[&img] = up;
    }

    // Render target view of back buffer `i`, used to clear it to black.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT i) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        return h;
    }

    // Gets the swap chain's back buffers and creates a render target view for each.
    // Called after creating or resizing the swap chain.
    bool get_buffers()
    {
        for (UINT i = 0; i < kBuffers; ++i)
        {
            if (FAILED(swap_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i]))))
                return false;
            dev_->CreateRenderTargetView(buffers_[i], nullptr, rtv(i));
        }
        return true;
    }

    // Drops the references to the back buffers (ResizeBuffers requires that).
    void release_buffers()
    {
        for (ID3D12Resource *&b : buffers_)
            release(b);
    }

    // Starts recording a new command list.
    void begin()
    {
        alloc_->Reset();
        list_->Reset(alloc_, nullptr);
    }

    // Records a transition of `r` from state `before` to `after`. Direct3D 12 requires the
    // program to declare how a resource is about to be used (copy source, render target...).
    void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        list_->ResourceBarrier(1, &b);
    }

    // Finishes the command list, runs it on the GPU and waits until it is done.
    bool submit(std::string &error)
    {
        if (FAILED(list_->Close()))
        {
            error = "command list error";
            return false;
        }
        ID3D12CommandList *lists[] = {list_};
        queue_->ExecuteCommandLists(1, lists);
        wait();
        return true;
    }

    // Blocks until the GPU has finished all work submitted so far (signals a fence and waits for it).
    void wait()
    {
        ++fence_value_;
        queue_->Signal(fence_, fence_value_);
        if (fence_->GetCompletedValue() < fence_value_)
        {
            fence_->SetEventOnCompletion(fence_value_, event_);
            WaitForSingleObject(event_, INFINITE);
        }
    }

    Options o_;
    IDXGIFactory4 *factory_ = nullptr;
    ID3D12Device *dev_ = nullptr;
    ID3D12CommandQueue *queue_ = nullptr;
    ID3D12CommandAllocator *alloc_ = nullptr;
    ID3D12GraphicsCommandList *list_ = nullptr;
    ID3D12Fence *fence_ = nullptr;
    UINT64 fence_value_ = 0;
    HANDLE event_ = nullptr;
    IDXGISwapChain3 *swap_ = nullptr;
    ID3D12DescriptorHeap *rtv_heap_ = nullptr;
    ID3D12Resource *buffers_[kBuffers] = {};
    std::map<const Image *, ID3D12Resource *> uploads_;
};
}

std::unique_ptr<Backend> make_d3d12()
{
    return std::make_unique<D3D12>();
}
