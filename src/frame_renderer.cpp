// Implementation of frame_renderer.h: the add-on's per-frame GPU work, through ReShade's
// API, with librashader (chain.h) running the capture preset and the user's preset.

#include "frame_renderer.h"
#include "capture.h"
#include "vulkan_support.h"

#include <d3d12.h>
#include <d3d9.h>

#include <utility>

using namespace reshade::api;

// A ring of kFramesInFlight private command lists librashader records into on Direct3D 12
// and Vulkan (see frame_renderer.h), used in turn. One is reused only once the GPU has
// finished what was recorded into it the last time, which a fence tells (a marker the GPU
// reaches after the list; ReShade's fences are not used because signalling one submits
// ReShade's commands at that point). So when frame N is recorded, the GPU has finished
// frame N - kFramesInFlight, as librashader needs (see kFramesInFlight in chain.h).
struct FrameRenderer::PrivateCommands
{
    // Implementations wait until the GPU has finished every list they submitted, then
    // release everything.
    virtual ~PrivateCommands() = default;
    // Waits until the next list of the ring is free and opens it for recording. Returns its
    // native handle (ID3D12GraphicsCommandList* or VkCommandBuffer), or 0 if it cannot be
    // opened.
    virtual uint64_t begin() = 0;
    // Records on the open list that `res` goes from state `before` to `after`.
    virtual void transition(resource res, resource_usage before, resource_usage after) = 0;
    // Closes the open list and submits it to the queue. Returns false if that fails.
    virtual bool submit() = 0;
};

// Direct3D 12: command allocators (the memory a command list records into) used in turn
// with one command list, and a fence (a counter the GPU sets when it reaches a point in the
// queue) that tells when the GPU has finished each allocator's list.
struct FrameRenderer::D3D12Commands final : FrameRenderer::PrivateCommands
{
    ID3D12CommandQueue *queue = nullptr; // not owned
    ID3D12CommandAllocator *allocators[kFramesInFlight] = {};
    UINT64 finished_at[kFramesInFlight] = {}; // fence value after the last list recorded with each
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 fence_value = 0;
    HANDLE event = nullptr;
    int current = 0;

    // Creates everything on `device`, submitting to `q`. Returns false if anything cannot
    // be created.
    bool create(ID3D12Device *device, ID3D12CommandQueue *q)
    {
        queue = q;
        for (ID3D12CommandAllocator *&a : allocators)
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
                return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr,
                                                   IID_PPV_ARGS(&list))) &&
               SUCCEEDED(list->Close()) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }

    // Blocks until the GPU has reached fence value `value`.
    void wait_for(UINT64 value)
    {
        if (fence != nullptr && fence->GetCompletedValue() < value)
        {
            fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, INFINITE);
        }
    }

    uint64_t begin() override
    {
        wait_for(finished_at[current]);
        if (FAILED(allocators[current]->Reset()) || FAILED(list->Reset(allocators[current], nullptr)))
            return 0;
        return reinterpret_cast<uint64_t>(list);
    }

    // ReShade's resource states as Direct3D 12 states (only those the renderer uses).
    static D3D12_RESOURCE_STATES state(resource_usage usage)
    {
        switch (usage)
        {
        case resource_usage::render_target:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case resource_usage::copy_source:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        default: // shader_resource, as ReShade maps it
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
    }

    void transition(resource res, resource_usage before, resource_usage after) override
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = reinterpret_cast<ID3D12Resource *>(res.handle);
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state(before);
        b.Transition.StateAfter = state(after);
        list->ResourceBarrier(1, &b);
    }

    bool submit() override
    {
        if (FAILED(list->Close()))
            return false;
        ID3D12CommandList *lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++fence_value);
        finished_at[current] = fence_value;
        current = (current + 1) % kFramesInFlight;
        return true;
    }

    ~D3D12Commands() override
    {
        wait_for(fence_value);
        if (list != nullptr)
            list->Release();
        for (ID3D12CommandAllocator *a : allocators)
            if (a != nullptr)
                a->Release();
        if (fence != nullptr)
            fence->Release();
        if (event != nullptr)
            CloseHandle(event);
    }
};

// Vulkan: command buffers from one command pool, used in turn, each with a fence the GPU
// signals when it has finished that buffer. Created through the Vulkan loader, so ReShade's
// layer registers the buffers like the game's (see frame_renderer.h).
struct FrameRenderer::VulkanCommands final : FrameRenderer::PrivateCommands
{
    VulkanFunctions vk;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffers[kFramesInFlight] = {};
    VkFence fences[kFramesInFlight] = {};
    // pending[i]: buffers[i] was submitted and fences[i] will be signalled when the GPU has
    // finished it; the fence has not been waited for yet.
    bool pending[kFramesInFlight] = {};
    int current = 0;

    // Waits until the GPU has finished buffers[i] if it was submitted, and resets its fence
    // for the next submission. Returns false if Vulkan reports an error (device lost).
    bool finish(int i)
    {
        if (!pending[i])
            return true;
        pending[i] = false;
        return vk.WaitForFences(device, 1, &fences[i], VK_TRUE, UINT64_MAX) == VK_SUCCESS &&
               vk.ResetFences(device, 1, &fences[i]) == VK_SUCCESS;
    }

    // Creates the pool (on the game's queue family), buffers and fences for the device and
    // queue in `chain`. Returns false if anything cannot be created.
    bool create(const ChainDevice &chain)
    {
        device = reinterpret_cast<VkDevice>(chain.device);
        queue = reinterpret_cast<VkQueue>(chain.queue);
        if (!vk.load(device, chain.get_device_proc_addr))
            return false;
        VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = chain.queue_family;
        if (vk.CreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
            return false;
        VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = kFramesInFlight;
        if (vk.AllocateCommandBuffers(device, &alloc, buffers) != VK_SUCCESS)
            return false;
        const VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (VkFence &f : fences)
            if (vk.CreateFence(device, &fence_info, nullptr, &f) != VK_SUCCESS)
                return false;
        return true;
    }

    // Records a barrier making everything written before it on the queue visible to
    // everything after it (all stages, all memory). At the start of a buffer it covers
    // ReShade's commands submitted just before (the snapshot copy); at the end, ReShade's
    // commands submitted after (the copy of the result into the frame). Barriers reach
    // across submissions on the same queue, so no ReShade barrier is relied on.
    void full_barrier()
    {
        VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vk.CmdPipelineBarrier(buffers[current], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              0, 1, &b, 0, nullptr, 0, nullptr);
    }

    uint64_t begin() override
    {
        if (!finish(current) || vk.ResetCommandBuffer(buffers[current], 0) != VK_SUCCESS)
            return 0;
        VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk.BeginCommandBuffer(buffers[current], &begin_info) != VK_SUCCESS)
            return 0;
        full_barrier();
        return reinterpret_cast<uint64_t>(buffers[current]);
    }

    // The image layout ReShade uses for each state the renderer uses (ReShade 6.8's
    // convert_usage_to_image_layout), so that ReShade's barriers and these agree.
    static VkImageLayout layout(resource_usage usage)
    {
        switch (usage)
        {
        case resource_usage::render_target:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case resource_usage::copy_source:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        default: // shader_resource
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    // Changes the image's layout, waiting for all earlier work (all stages, all memory):
    // simple and always correct; a few barriers per frame cost nothing noticeable.
    void transition(resource res, resource_usage before, resource_usage after) override
    {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = layout(before);
        b.newLayout = layout(after);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = reinterpret_cast<VkImage>(res.handle);
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vk.CmdPipelineBarrier(buffers[current], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &b);
    }

    bool submit() override
    {
        full_barrier();
        VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &buffers[current];
        const bool ok = vk.EndCommandBuffer(buffers[current]) == VK_SUCCESS &&
                        vk.QueueSubmit(queue, 1, &submit_info, fences[current]) == VK_SUCCESS;
        pending[current] = ok;
        current = (current + 1) % kFramesInFlight;
        return ok;
    }

    ~VulkanCommands() override
    {
        if (vk.DestroyFence == nullptr)
            return; // load() failed: nothing was created
        for (int i = 0; i < int(kFramesInFlight); ++i)
            if (fences[i] != VK_NULL_HANDLE)
            {
                finish(i);
                vk.DestroyFence(device, fences[i], nullptr);
            }
        if (pool != VK_NULL_HANDLE)
            vk.DestroyCommandPool(device, pool, nullptr); // also frees the buffers
    }
};

// Defined here, where the command rings are complete (they are only declared in the header).
FrameRenderer::FrameRenderer() = default;

FrameRenderer::~FrameRenderer()
{
    shutdown();
}

bool FrameRenderer::chain_device(device *dev, command_queue *queue, ChainDevice &out, std::string &error)
{
    switch (dev->get_api())
    {
    case device_api::d3d11:
        out = {GraphicsApi::d3d11, dev->get_native()};
        break;
    case device_api::d3d9:
        out = {GraphicsApi::d3d9, dev->get_native()};
        break;
    case device_api::d3d12:
        out = {GraphicsApi::d3d12, dev->get_native()};
        break;
    case device_api::opengl:
        out = {GraphicsApi::opengl, dev->get_native()};
        break;
    case device_api::vulkan:
    {
        // librashader also needs objects ReShade does not give add-ons (vulkan_support.h).
        uint8_t luid[8] = {};
        const bool has_luid = dev->get_property(device_properties::adapter_luid, luid);
        VulkanHandles vh;
        if (!vulkan_handles(has_luid ? luid : nullptr, uint32_t(queue->get_type()), vh, error))
        {
            error = "Vulkan: " + error;
            return false;
        }
        out = {GraphicsApi::vulkan, dev->get_native()};
        out.instance = vh.instance;
        out.physical_device = vh.physical_device;
        out.queue = queue->get_native();
        out.queue_family = vh.queue_family;
        out.get_instance_proc_addr = vh.get_instance_proc_addr;
        out.get_device_proc_addr = vh.get_device_proc_addr;
        break;
    }
    default: // Direct3D 10: librashader has no runtime for it
        error = "Direct3D 10 is not supported (librashader cannot run on it).";
        return false;
    }
    if (!ShaderChain::supports(out.api))
    {
        static const char *const names[] = {"Direct3D 9", "Direct3D 11", "Direct3D 12", "OpenGL", "Vulkan"};
        error = std::string(names[int(out.api)]) + " is not supported yet.";
        return false;
    }
    return true;
}

bool FrameRenderer::supported_format(format f)
{
    switch (format_to_default_typed(f, 0))
    {
    case format::r8g8b8a8_unorm:
    case format::b8g8r8a8_unorm:
    case format::b8g8r8x8_unorm:
    case format::r10g10b10a2_unorm:
        return true;
    default:
        return false;
    }
}

bool FrameRenderer::init(device *dev, command_queue *queue, std::string &error)
{
    shutdown();
    if (!chain_device(dev, queue, chain_device_, error))
        return false;
    device_ = dev;
    queue_ = queue;
    const std::string preset = capture_preset_path(chain_device_.api == GraphicsApi::d3d9, error);
    if (preset.empty() || !capture_.create(chain_device_, preset, error))
    {
        error = "could not load the capture shader: " + error;
        shutdown();
        return false;
    }
    return true;
}

void FrameRenderer::wait_idle()
{
    if (queue_ != nullptr)
        queue_->wait_idle();
}

void FrameRenderer::shutdown()
{
    // The GPU may still be using the textures and compiled presets: wait for it first.
    wait_idle();
    private_.reset();
    private_open_ = false;
    queue_ = nullptr;
    capture_.destroy();
    capture_grid_ = PixelGrid();
    if (device_ != nullptr)
    {
        destroy(snap_, snap_srv_, snap_unused_);
        destroy(native_, native_rtv_, native_srv_);
        destroy(out_, out_rtv_, out_unused_);
    }
    snap_w_ = snap_h_ = native_w_ = native_h_ = out_w_ = out_h_ = 0;
    snap_format_ = out_format_ = format::unknown;
    device_ = nullptr;
}

void FrameRenderer::destroy(resource &res, resource_view &view1, resource_view &view2)
{
    for (resource_view *v : {&view1, &view2})
    {
        if (v->handle != 0)
            device_->destroy_resource_view(*v);
        *v = {};
    }
    if (res.handle != 0)
        device_->destroy_resource(res);
    res = {};
}

ChainImage FrameRenderer::chain_image(resource res, resource_view view, format fmt, uint32_t width,
                                      uint32_t height) const
{
    if (chain_device_.api == GraphicsApi::opengl)
    {
        // ReShade's OpenGL handles hold the object's name in their low 32 bits. The view is
        // used: it is the texture seen with the right format (a texture view when ReShade
        // had to make one). librashader wants the sized internal format.
        uint32_t gl_format = 0x8058; // GL_RGBA8
        if (format_to_default_typed(fmt, 0) == format::r10g10b10a2_unorm)
            gl_format = 0x8059; // GL_RGB10_A2
        return ChainImage{view.handle & 0xFFFFFFFF, view.handle, gl_format, width, height};
    }
    if (chain_device_.api == GraphicsApi::vulkan)
    {
        // ReShade's Vulkan handles are the VkImage itself. librashader wants the VkFormat,
        // as ReShade 6.8 maps these formats (convert_format).
        VkFormat vk_format = VK_FORMAT_R8G8B8A8_UNORM;
        switch (format_to_default_typed(fmt, 0))
        {
        case format::b8g8r8a8_unorm:
        case format::b8g8r8x8_unorm:
            vk_format = VK_FORMAT_B8G8R8A8_UNORM;
            break;
        case format::r10g10b10a2_unorm:
            vk_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            break;
        default: // r8g8b8a8_unorm
            break;
        }
        return ChainImage{res.handle, view.handle, uint32_t(vk_format), width, height};
    }
    // Direct3D 11 takes views; Direct3D 12 the resource, a descriptor and the format;
    // Direct3D 9 the input texture and the output's render target surface (the view).
    return ChainImage{res.handle, view.handle, uint32_t(fmt), width, height};
}

format FrameRenderer::copy_format(format frame_format) const
{
    return chain_device_.api == GraphicsApi::d3d9 ? format_to_default_typed(frame_format, 0)
                                                  : format_to_typeless(frame_format);
}

format FrameRenderer::native_format() const
{
    return chain_device_.api == GraphicsApi::d3d9 ? format::b8g8r8a8_unorm : format::r8g8b8a8_unorm;
}

// The snapshot is created typeless (only the bytes per pixel fixed, their meaning left to
// each view) so it can be read as plain values even when the frame is sRGB; copies
// between a typed and a typeless texture of the same family are allowed. Direct3D 9 has
// no typeless formats (and handles sRGB elsewhere), so there it has the frame's format.
bool FrameRenderer::ensure_snapshot(const resource_desc &frame, std::string &error)
{
    if (snap_.handle != 0 && snap_w_ == frame.texture.width && snap_h_ == frame.texture.height &&
        snap_format_ == frame.texture.format)
        return true;
    destroy(snap_, snap_srv_, snap_unused_);
    const resource_desc desc(frame.texture.width, frame.texture.height, 1, 1, copy_format(frame.texture.format), 1,
                             memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest);
    if (!device_->create_resource(desc, nullptr, resource_usage::shader_resource, &snap_) ||
        !device_->create_resource_view(snap_, resource_usage::shader_resource,
                                       resource_view_desc(format_to_default_typed(frame.texture.format, 0)), &snap_srv_))
    {
        destroy(snap_, snap_srv_, snap_unused_);
        error = "could not create the frame snapshot texture";
        return false;
    }
    snap_w_ = frame.texture.width;
    snap_h_ = frame.texture.height;
    snap_format_ = frame.texture.format;
    return true;
}

bool FrameRenderer::ensure_native(uint32_t w, uint32_t h, std::string &error)
{
    if (native_.handle != 0 && native_w_ == w && native_h_ == h)
        return true;
    destroy(native_, native_rtv_, native_srv_);
    const resource_desc desc(w, h, 1, 1, native_format(), 1, memory_heap::default_,
                             resource_usage::render_target | resource_usage::shader_resource);
    if (!device_->create_resource(desc, nullptr, resource_usage::shader_resource, &native_) ||
        !device_->create_resource_view(native_, resource_usage::render_target,
                                       resource_view_desc(native_format()), &native_rtv_) ||
        !device_->create_resource_view(native_, resource_usage::shader_resource,
                                       resource_view_desc(native_format()), &native_srv_))
    {
        destroy(native_, native_rtv_, native_srv_);
        error = "could not create the native image texture";
        return false;
    }
    native_w_ = w;
    native_h_ = h;
    return true;
}

// The output texture is in the frame's format family (typeless, drawn through a plain
// view) so it can be copied into the frame.
bool FrameRenderer::ensure_output(uint32_t w, uint32_t h, format frame_format, std::string &error)
{
    if (out_.handle != 0 && out_w_ == w && out_h_ == h && out_format_ == frame_format)
        return true;
    destroy(out_, out_rtv_, out_unused_);
    const resource_desc desc(w, h, 1, 1, copy_format(frame_format), 1, memory_heap::default_,
                             resource_usage::render_target | resource_usage::copy_source);
    if (!device_->create_resource(desc, nullptr, resource_usage::copy_source, &out_) ||
        !device_->create_resource_view(out_, resource_usage::render_target,
                                       resource_view_desc(format_to_default_typed(frame_format, 0)), &out_rtv_))
    {
        destroy(out_, out_rtv_, out_unused_);
        error = "could not create the output texture";
        return false;
    }
    out_w_ = w;
    out_h_ = h;
    out_format_ = frame_format;
    return true;
}

bool FrameRenderer::snapshot(command_list *cmd, resource frame, std::string &error)
{
    const resource_desc fd = device_->get_resource_desc(frame);
    if (!supported_format(fd.texture.format))
    {
        error = "unsupported back buffer format " + std::to_string(int(fd.texture.format)) +
                " (HDR is not supported yet)";
        return false;
    }
    if (fd.texture.samples > 1)
    {
        error = "multisampled back buffers are not supported";
        return false;
    }
    if (!ensure_snapshot(fd, error))
        return false;
    const resource resources[2] = {frame, snap_};
    const resource_usage before[2] = {resource_usage::render_target, resource_usage::shader_resource};
    const resource_usage during[2] = {resource_usage::copy_source, resource_usage::copy_dest};
    cmd->barrier(2, resources, before, during);
    cmd->copy_resource(frame, snap_);
    cmd->barrier(2, resources, during, before);
    return true;
}

bool FrameRenderer::begin_librashader(command_list *cmd, command_queue *queue, uint64_t &commands, std::string &error)
{
    const GraphicsApi api = chain_device_.api;
    if (api == GraphicsApi::d3d9)
    {
        // End the scene ReShade renders effects in, if one is open (EndScene succeeds only
        // then), so librashader can begin its own (see the class comment).
        d3d9_scene_ended_ = SUCCEEDED(reinterpret_cast<IDirect3DDevice9 *>(device_->get_native())->EndScene());
    }
    if (api != GraphicsApi::d3d12 && api != GraphicsApi::vulkan)
    {
        commands = cmd->get_native();
        return true;
    }
    // The private list is submitted right after the immediate command list's pending
    // commands; work recorded on another list would end up out of order.
    if (cmd != queue->get_immediate_command_list())
    {
        error = "effects are being rendered on another add-on's command list, which is not supported on "
                "Direct3D 12 and Vulkan";
        return false;
    }
    if (private_ == nullptr)
    {
        bool ok = false;
        if (api == GraphicsApi::d3d12)
        {
            auto p = std::make_unique<D3D12Commands>();
            ok = p->create(reinterpret_cast<ID3D12Device *>(device_->get_native()),
                           reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
            private_ = std::move(p);
        }
        else
        {
            auto p = std::make_unique<VulkanCommands>();
            ok = p->create(chain_device_);
            private_ = std::move(p);
        }
        if (!ok)
        {
            private_.reset();
            error = "could not create a private command list";
            return false;
        }
    }
    // ReShade's commands so far (the snapshot, the barriers) run first.
    queue->flush_immediate_command_list();
    commands = private_->begin();
    if (commands == 0)
    {
        error = "could not open a private command list";
        return false;
    }
    private_open_ = true;
    return true;
}

bool FrameRenderer::end_librashader(std::string &error)
{
    if (d3d9_scene_ended_)
    {
        d3d9_scene_ended_ = false;
        reinterpret_cast<IDirect3DDevice9 *>(device_->get_native())->BeginScene();
    }
    if (!private_open_)
        return true;
    private_open_ = false;
    if (!private_->submit())
    {
        error = "could not submit the private command list";
        return false;
    }
    return true;
}

void FrameRenderer::transition(resource res, resource_usage before, resource_usage after)
{
    if (private_open_)
        private_->transition(res, before, after);
}

bool FrameRenderer::render(command_list *cmd, command_queue *queue, const PixelGrid &grid, ShaderChain &chain,
                           resource dst, uint64_t frame_count, std::string &error)
{
    // Refuse to run without a snapshot or a valid grid, or with a grid whose rectangle
    // reaches outside the snapshot.
    if (snap_.handle == 0 || !grid.valid)
    {
        error = "nothing to render";
        return false;
    }
    if (grid.rect_x < 0 || grid.rect_y < 0 || uint32_t(grid.rect_x + grid.rect_w) > snap_w_ ||
        uint32_t(grid.rect_y + grid.rect_h) > snap_h_)
    {
        error = "pixel grid does not fit the frame";
        return false;
    }
    const uint32_t nw = uint32_t(grid.native_w), nh = uint32_t(grid.native_h);
    const uint32_t rw = uint32_t(grid.rect_w), rh = uint32_t(grid.rect_h);
    const format dst_format = device_->get_resource_desc(dst).texture.format;
    if (!ensure_native(nw, nh, error) || !ensure_output(rw, rh, dst_format, error) || !set_capture_grid(grid, error))
        return false;

    // Steps 1 and 2 run librashader, recorded where it may change any state (see the class
    // comment). Both textures are switched to render targets here, on ReShade's list; the
    // switches between and after the two steps are recorded next to them.
    cmd->barrier(native_, resource_usage::shader_resource, resource_usage::render_target);
    cmd->barrier(out_, resource_usage::copy_source, resource_usage::render_target);
    uint64_t commands = 0;
    if (!begin_librashader(cmd, queue, commands, error))
    {
        cmd->barrier(native_, resource_usage::render_target, resource_usage::shader_resource);
        cmd->barrier(out_, resource_usage::render_target, resource_usage::copy_source);
        return false;
    }

    // Step 1: rebuild the native picture: run the capture preset on the snapshot, into
    // native_ (native_w x native_h). Its one pass writes each native pixel from the frame
    // pixel at the centre of that pixel's block (capture.slang).
    bool ok = capture_.frame(commands,
                             chain_image(snap_, snap_srv_, format_to_default_typed(snap_format_, 0), snap_w_, snap_h_),
                             chain_image(native_, native_rtv_, native_format(), nw, nh), 0, 0, int(nw), int(nh),
                             frame_count, error);
    transition(native_, resource_usage::render_target, resource_usage::shader_resource);

    // Step 2: run the user's preset on the native picture, into out_, which is exactly the
    // size of the rectangle (librashader clears its whole output, so drawing straight into
    // the frame would erase what surrounds the game picture).
    ok = ok && chain.frame(commands, chain_image(native_, native_srv_, native_format(), nw, nh),
                           chain_image(out_, out_rtv_, format_to_default_typed(dst_format, 0), rw, rh), 0, 0, int(rw),
                           int(rh), frame_count, error);
    transition(out_, resource_usage::render_target, resource_usage::copy_source);
    // Submit even after a failure: the list is open, and the transitions above must run
    // for the states to be what the next frame expects.
    std::string submit_error;
    if (!end_librashader(submit_error) && ok)
    {
        error = submit_error;
        ok = false;
    }
    if (!ok)
        return false;

    // Step 3: copy out_ into the frame at the rectangle's position; the rest of the frame
    // stays as it was.
    const subresource_box src_box = {0, 0, 0, rw, rh, 1};
    const subresource_box dst_box = {uint32_t(grid.rect_x), uint32_t(grid.rect_y), 0,
                                     uint32_t(grid.rect_x) + rw, uint32_t(grid.rect_y) + rh, 1};
    cmd->barrier(dst, resource_usage::render_target, resource_usage::copy_dest);
    cmd->copy_texture_region(out_, 0, &src_box, dst, 0, &dst_box);
    cmd->barrier(dst, resource_usage::copy_dest, resource_usage::render_target);
    return true;
}

bool FrameRenderer::set_capture_grid(const PixelGrid &grid, std::string &error)
{
    if (grid.same_as(capture_grid_))
        return true;
    const std::pair<const char *, int> values[] = {
        {"rra_rect_x", grid.rect_x}, {"rra_rect_y", grid.rect_y},     {"rra_rect_w", grid.rect_w},
        {"rra_rect_h", grid.rect_h}, {"rra_native_w", grid.native_w}, {"rra_native_h", grid.native_h}};
    for (const auto &[name, value] : values)
        if (!capture_.set_param(name, float(value), error))
            return false;
    capture_grid_ = grid;
    return true;
}
