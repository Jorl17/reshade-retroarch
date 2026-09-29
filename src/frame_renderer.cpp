// Implementation of frame_renderer.h: the add-on's per-frame GPU work, through ReShade's
// API, with librashader (chain.h) running the capture preset and the user's preset.

#include "frame_renderer.h"
#include "capture.h"

#include <utility>

using namespace reshade::api;

bool FrameRenderer::chain_device(device *dev, ChainDevice &out, std::string &error)
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
        out = {GraphicsApi::vulkan, dev->get_native()};
        break;
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

bool FrameRenderer::init(device *dev, std::string &error)
{
    shutdown();
    if (!chain_device(dev, chain_device_, error))
        return false;
    device_ = dev;
    const std::string preset = capture_preset_path(error);
    if (preset.empty() || !capture_.create(chain_device_, preset, error))
    {
        error = "could not load the capture shader: " + error;
        shutdown();
        return false;
    }
    return true;
}

void FrameRenderer::shutdown()
{
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
    // Direct3D 11 takes views; the other APIs are added with their support.
    return ChainImage{res.handle, view.handle, uint32_t(fmt), width, height};
}

// The snapshot is created typeless (only the bytes per pixel fixed, their meaning left to
// each view) so it can be read as plain values even when the frame is sRGB; copies
// between a typed and a typeless texture of the same family are allowed.
bool FrameRenderer::ensure_snapshot(const resource_desc &frame, std::string &error)
{
    if (snap_.handle != 0 && snap_w_ == frame.texture.width && snap_h_ == frame.texture.height &&
        snap_format_ == frame.texture.format)
        return true;
    destroy(snap_, snap_srv_, snap_unused_);
    const resource_desc desc(frame.texture.width, frame.texture.height, 1, 1, format_to_typeless(frame.texture.format), 1,
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
    const resource_desc desc(w, h, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::default_,
                             resource_usage::render_target | resource_usage::shader_resource);
    if (!device_->create_resource(desc, nullptr, resource_usage::shader_resource, &native_) ||
        !device_->create_resource_view(native_, resource_usage::render_target,
                                       resource_view_desc(format::r8g8b8a8_unorm), &native_rtv_) ||
        !device_->create_resource_view(native_, resource_usage::shader_resource,
                                       resource_view_desc(format::r8g8b8a8_unorm), &native_srv_))
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
    const resource_desc desc(w, h, 1, 1, format_to_typeless(frame_format), 1, memory_heap::default_,
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

bool FrameRenderer::render(command_list *cmd, const PixelGrid &grid, ShaderChain &chain, resource dst,
                           uint64_t frame_count, std::string &error)
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
    const uint64_t native_cmd = cmd->get_native();

    // Step 1: rebuild the native picture: run the capture preset on the snapshot, into
    // native_ (native_w x native_h). Its one pass writes each native pixel from the frame
    // pixel at the centre of that pixel's block (capture.slang).
    cmd->barrier(native_, resource_usage::shader_resource, resource_usage::render_target);
    const bool captured =
        capture_.frame(native_cmd, chain_image(snap_, snap_srv_, format_to_default_typed(snap_format_, 0), snap_w_, snap_h_),
                       chain_image(native_, native_rtv_, format::r8g8b8a8_unorm, nw, nh), 0, 0, int(nw), int(nh),
                       frame_count, error);
    cmd->barrier(native_, resource_usage::render_target, resource_usage::shader_resource);
    if (!captured)
        return false;

    // Step 2: run the user's preset on the native picture, into out_, which is exactly the
    // size of the rectangle (librashader clears its whole output, so drawing straight into
    // the frame would erase what surrounds the game picture).
    cmd->barrier(out_, resource_usage::copy_source, resource_usage::render_target);
    const bool drawn = chain.frame(native_cmd, chain_image(native_, native_srv_, format::r8g8b8a8_unorm, nw, nh),
                                   chain_image(out_, out_rtv_, format_to_default_typed(dst_format, 0), rw, rh), 0, 0,
                                   int(rw), int(rh), frame_count, error);
    cmd->barrier(out_, resource_usage::render_target, resource_usage::copy_source);
    if (!drawn)
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
