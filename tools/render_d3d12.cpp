// Implementation of render_d3d12.h: the add-on's per-frame work (FrameRenderer) with plain
// Direct3D 12, for the offline tool render_png.

#include "render_d3d12.h"
#include "capture.h"
#include "chain.h"
#include "librashader_api.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
// Everything one render needs: the device, a command list that is run and waited for after
// each recording (simple, and the offline tool has no frames to overlap), and helpers for
// textures and their views.
struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    HANDLE event = nullptr;
    // Descriptor heaps the CPU writes views into; librashader copies them into its own.
    ComPtr<ID3D12DescriptorHeap> srv_heap, rtv_heap;
    UINT srv_size = 0, rtv_size = 0, srv_used = 0, rtv_used = 0;

    ~Gpu()
    {
        if (event != nullptr)
            CloseHandle(event);
    }

    // Creates the device (default GPU) and everything above. Returns false on failure.
    bool create()
    {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        D3D12_DESCRIPTOR_HEAP_DESC sd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC rd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
            FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
            FAILED(device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&srv_heap))) ||
            FAILED(device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtv_heap))))
            return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        srv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        return event != nullptr;
    }

    // Opens the command list for recording.
    bool begin() { return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)); }

    // Closes the command list, runs it and waits until the GPU has finished it.
    bool run()
    {
        if (FAILED(list->Close()))
            return false;
        ID3D12CommandList *lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++fence_value)))
            return false;
        if (fence->GetCompletedValue() < fence_value)
        {
            fence->SetEventOnCompletion(fence_value, event);
            WaitForSingleObject(event, INFINITE);
        }
        return true;
    }

    // Creates a `w` x `h` 8-bit RGBA texture in `state`, allowed as a render target when
    // `render_target`.
    ComPtr<ID3D12Resource> texture(UINT w, UINT h, bool render_target, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

    // Creates a buffer of `size` bytes on `type` (upload or readback) in `state`.
    ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap = {type};
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

    // Writes a shader resource view / render target view of `tex` into the next free slot
    // of its heap and returns the CPU descriptor handle.
    D3D12_CPU_DESCRIPTOR_HANDLE srv(ID3D12Resource *tex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(srv_used++) * srv_size;
        device->CreateShaderResourceView(tex, nullptr, h);
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(ID3D12Resource *tex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(rtv_used++) * rtv_size;
        device->CreateRenderTargetView(tex, nullptr, h);
        return h;
    }

    // Records a state transition of `res`.
    void barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
    }
};

// The state a texture is in while shaders read it (as ReShade's shader_resource state).
constexpr D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

bool render_d3d12(std::vector<uint8_t> &rgba, UINT w, UINT h, const PixelGrid &grid, const std::string &preset_path,
                  const std::vector<std::pair<std::string, float>> &sets, int frames, const std::wstring &dxc_dir,
                  std::string &error)
{
    if (!libra::load_d3d12_compiler(dxc_dir, error))
        return false;
    Gpu gpu;
    if (!gpu.create())
    {
        error = "could not create a Direct3D 12 device";
        return false;
    }
    const UINT nw = UINT(grid.native_w), nh = UINT(grid.native_h), rw = UINT(grid.rect_w), rh = UINT(grid.rect_h);

    // The frame (the add-on's snapshot), the native picture and the preset's output, with
    // the views librashader is given, as in FrameRenderer.
    ComPtr<ID3D12Resource> frame = gpu.texture(w, h, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Resource> native = gpu.texture(nw, nh, true, kRead);
    ComPtr<ID3D12Resource> out = gpu.texture(rw, rh, true, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!frame || !native || !out)
    {
        error = "could not create the Direct3D 12 textures";
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE frame_srv = gpu.srv(frame.Get()), native_srv = gpu.srv(native.Get());
    const D3D12_CPU_DESCRIPTOR_HANDLE native_rtv = gpu.rtv(native.Get()), out_rtv = gpu.rtv(out.Get());

    // Upload the picture into the frame texture.
    D3D12_RESOURCE_DESC fd = frame->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 upload_size = 0;
    gpu.device->GetCopyableFootprints(&fd, 0, 1, 0, &fp, nullptr, nullptr, &upload_size);
    ComPtr<ID3D12Resource> upload = gpu.buffer(upload_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    if (!upload || FAILED(upload->Map(0, nullptr, &mapped)))
    {
        error = "could not upload the picture";
        return false;
    }
    for (UINT y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t *>(mapped) + fp.Offset + size_t(y) * fp.Footprint.RowPitch,
                    rgba.data() + size_t(y) * w * 4, size_t(w) * 4);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {frame.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = fp;
    if (!gpu.begin())
    {
        error = "could not record Direct3D 12 commands";
        return false;
    }
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    gpu.barrier(frame.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    if (!gpu.run())
    {
        error = "could not upload the picture";
        return false;
    }

    // The capture preset, given the grid (as FrameRenderer::set_capture_grid), and the preset.
    const ChainDevice dev = {GraphicsApi::d3d12, reinterpret_cast<uint64_t>(gpu.device.Get())};
    ShaderChain capture, chain;
    const std::string capture_path = capture_preset_path(false, error);
    if (capture_path.empty() || !capture.create(dev, capture_path, error) || !chain.create(dev, preset_path, error))
        return false;
    const std::pair<const char *, int> grid_values[] = {
        {"rra_rect_x", grid.rect_x}, {"rra_rect_y", grid.rect_y},     {"rra_rect_w", grid.rect_w},
        {"rra_rect_h", grid.rect_h}, {"rra_native_w", grid.native_w}, {"rra_native_h", grid.native_h}};
    for (const auto &[name, value] : grid_values)
        if (!capture.set_param(name, float(value), error))
            return false;
    for (const auto &[name, value] : sets)
        if (!chain.set_param(name, value, error))
            return false;

    // Each frame, as FrameRenderer::render: the capture preset draws the native picture from
    // the frame, then the preset draws from the native picture into `out`.
    const auto image = [](ID3D12Resource *r, D3D12_CPU_DESCRIPTOR_HANDLE view, UINT iw, UINT ih) {
        return ChainImage{reinterpret_cast<uint64_t>(r), uint64_t(view.ptr), uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM), iw, ih};
    };
    for (int i = 0; i < frames; ++i)
    {
        if (!gpu.begin())
        {
            error = "could not record Direct3D 12 commands";
            return false;
        }
        const uint64_t cmd = reinterpret_cast<uint64_t>(gpu.list.Get());
        gpu.barrier(native.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
        bool ok = capture.frame(cmd, image(frame.Get(), frame_srv, w, h), image(native.Get(), native_rtv, nw, nh), 0, 0,
                                int(nw), int(nh), uint64_t(i), error);
        gpu.barrier(native.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
        ok = ok && chain.frame(cmd, image(native.Get(), native_srv, nw, nh), image(out.Get(), out_rtv, rw, rh), 0, 0,
                               int(rw), int(rh), uint64_t(i), error);
        if (!gpu.run() || !ok)
        {
            if (error.empty())
                error = "could not run Direct3D 12 commands";
            return false;
        }
    }

    // Read `out` back and write it into the picture at the grid's rectangle.
    D3D12_RESOURCE_DESC od = out->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT op = {};
    UINT64 read_size = 0;
    gpu.device->GetCopyableFootprints(&od, 0, 1, 0, &op, nullptr, nullptr, &read_size);
    ComPtr<ID3D12Resource> readback = gpu.buffer(read_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback || !gpu.begin())
    {
        error = "could not read the result back";
        return false;
    }
    gpu.barrier(out.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION rsrc = {out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION rdst = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    rdst.PlacedFootprint = op;
    gpu.list->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
    if (!gpu.run() || FAILED(readback->Map(0, nullptr, &mapped)))
    {
        error = "could not read the result back";
        return false;
    }
    for (UINT y = 0; y < rh; ++y)
        std::memcpy(rgba.data() + (size_t(grid.rect_y) + y) * w * 4 + size_t(grid.rect_x) * 4,
                    static_cast<const uint8_t *>(mapped) + op.Offset + size_t(y) * op.Footprint.RowPitch,
                    size_t(rw) * 4);
    readback->Unmap(0, nullptr);
    return true;
}
