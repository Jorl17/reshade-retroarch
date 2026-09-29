#pragma once
// The add-on's renderer: takes the frame the game is about to show, rebuilds the game's
// native (original, low-resolution) picture from it, runs the user's RetroArch preset on
// that picture and writes the result back into the frame. It does the same job as
// Renderer (renderer.h), which the offline tool render_png uses on Direct3D 11, but all
// GPU work here goes through ReShade's API (textures, copies, state changes), so the same
// code runs on every graphics API; only the librashader calls differ per API (chain.h).
// Used only inside ReShade (addon.cpp).

#include "chain.h"
#include "grid_detect.h"

#include <reshade_api_device.hpp>

#include <memory>
#include <string>

// Turns a game frame into a shader-processed frame. Each frame the add-on calls:
//  1. snapshot(): copy the frame into a texture owned by the renderer, because step 2
//     writes into the frame itself (the grid detector also reads this copy).
//  2. render(): rebuild the native picture from the copy with the capture preset
//     (capture.h), run the user's preset on it, and copy the result into the frame over
//     the game picture's rectangle. Pixels outside that rectangle are never touched.
//
// Resource states (how the GPU is told a texture will be used next; required on D3D12
// and Vulkan, ignored elsewhere): between calls the snapshot and the native picture are in
// the shader_resource state and the output texture in copy_source; the frame is expected
// in render_target (as during ReShade's effect rendering) and left there.
//
// Where librashader records its work: on Direct3D 11, straight on ReShade's command list
// (ReShade restores the game's state afterwards). On Direct3D 12, on a private command
// list submitted to the same queue right after ReShade's pending commands, because
// librashader binds its own descriptor heaps and ReShade assumes its own stay bound on
// its command list.
//
// Owns its textures, recreates them when sizes or formats change, and releases them in
// shutdown() or the destructor (after the GPU has finished with them). Not thread-safe:
// calls must not overlap.
class FrameRenderer
{
public:
    FrameRenderer();
    FrameRenderer(const FrameRenderer &) = delete;
    FrameRenderer &operator=(const FrameRenderer &) = delete;
    ~FrameRenderer(); // calls shutdown()

    // Fills `out` with what librashader needs to know about `device` (its API and native
    // device). Returns false and sets `error` (a message for the user) when the device's
    // graphics API is not supported (yet).
    static bool chain_device(reshade::api::device *device, ChainDevice &out, std::string &error);

    // Prepares the renderer for `device`: loads the capture preset for its graphics API.
    // librashader must already be loaded (librashader_api.h). Releases anything from an
    // earlier init() first. Returns false and sets `error` on failure.
    bool init(reshade::api::device *device, std::string &error);
    // Releases every GPU object and forgets the device. Safe to call more than once.
    void shutdown();

    // Copies `frame` (the texture the game's picture is in, normally the back buffer; in
    // the render_target state) into the snapshot, recording the commands on `cmd`.
    // Returns false and sets `error` when the frame's format is not supported (see
    // supported_format()), the frame is multisampled, or the copy cannot be created.
    bool snapshot(reshade::api::command_list *cmd, reshade::api::resource frame, std::string &error);
    // The texture made by the last snapshot() (in the shader_resource state), or {0}.
    reshade::api::resource snapshot_resource() const { return snap_; }

    // Draws the shader-processed picture into `dst` (normally the frame given to
    // snapshot(), in the render_target state): rebuilds the native picture described by
    // `grid` from the last snapshot, runs `chain` on it into a texture the size of grid's
    // rectangle, and copies that into `dst` at the rectangle's position. `frame_count` is
    // the frame number the shaders see; it should go up by one each frame. Records the
    // commands on `cmd`, which must be `queue`'s immediate command list (see the class
    // comment for Direct3D 12). Returns false and sets `error` when there is no snapshot,
    // `grid` is invalid or reaches outside the snapshot, or a texture or a preset fails.
    bool render(reshade::api::command_list *cmd, reshade::api::command_queue *queue, const PixelGrid &grid,
                ShaderChain &chain, reshade::api::resource dst, uint64_t frame_count, std::string &error);

    // True for the frame formats the capture can read: 8 bits per channel RGBA, BGRA or
    // BGRX (plain, sRGB or typeless) and 10-bit RGB with 2-bit alpha. False for everything
    // else, notably the 16-bit float formats used for HDR.
    static bool supported_format(reshade::api::format f);

private:
    // The image description librashader needs for `resource` with view `view` of the given
    // format and size (see ChainImage in chain.h), for the renderer's graphics API.
    ChainImage chain_image(reshade::api::resource resource, reshade::api::resource_view view,
                           reshade::api::format format, uint32_t width, uint32_t height) const;
    // Make sure the snapshot, the native picture and the output texture exist with the
    // needed size and format, recreating them (and their views) when either changed.
    // Return false and set `error` if one cannot be created.
    bool ensure_snapshot(const reshade::api::resource_desc &frame, std::string &error);
    bool ensure_native(uint32_t w, uint32_t h, std::string &error);
    bool ensure_output(uint32_t w, uint32_t h, reshade::api::format frame_format, std::string &error);
    // Passes the grid's rectangle and native size to the capture preset as its parameters,
    // when they differ from the last ones passed. Returns false and sets `error` on failure.
    bool set_capture_grid(const PixelGrid &grid, std::string &error);
    // Format for the renderer's copies of a frame of format `frame_format`: its typeless
    // family where the API has typeless formats, the frame's own format on Direct3D 9.
    reshade::api::format copy_format(reshade::api::format frame_format) const;
    // Format of the native picture: 8-bit RGBA, or BGRA on Direct3D 9, where RGBA textures
    // usually cannot be drawn into.
    reshade::api::format native_format() const;
    // Destroy one texture and its views (any of which may be {0}) and set them to {0}.
    void destroy(reshade::api::resource &res, reshade::api::resource_view &view1, reshade::api::resource_view &view2);

    // Returns, in `commands`, the native command recorder librashader should use this
    // frame (see the class comment): ReShade's list on Direct3D 11; on Direct3D 12 it first
    // submits ReShade's pending commands, then opens the private list. Returns false and
    // sets `error` if the private list cannot be created.
    bool begin_librashader(reshade::api::command_list *cmd, reshade::api::command_queue *queue, uint64_t &commands,
                           std::string &error);
    // Submits the private list on Direct3D 12 (after begin_librashader); nothing elsewhere.
    void end_librashader(reshade::api::command_queue *queue);
    // Records, on `commands` (from begin_librashader), that `res` goes from `before` to
    // `after`. Needed on Direct3D 12's private list, where ReShade's barrier() cannot be
    // used; a no-op on Direct3D 11.
    void transition(uint64_t commands, reshade::api::resource res, reshade::api::resource_usage before,
                    reshade::api::resource_usage after);

    // The private Direct3D 12 command list and what keeps it safe to reuse (defined in the
    // .cpp file, so this header does not need Direct3D 12's).
    struct PrivateCommands;
    std::unique_ptr<PrivateCommands> private_;
    // The queue of the last render(), to wait for the GPU before releasing textures.
    reshade::api::command_queue *queue_ = nullptr;

    reshade::api::device *device_ = nullptr; // from init(); not owned
    ChainDevice chain_device_;
    // The capture preset compiled for the device, and the grid it was last given.
    ShaderChain capture_;
    PixelGrid capture_grid_;

    // The snapshot: a copy of the frame, created typeless (see ensure_snapshot) and read by
    // the capture preset through snap_srv_ as plain (non-sRGB) values.
    reshade::api::resource snap_ = {};
    reshade::api::resource_view snap_srv_ = {}, snap_unused_ = {};
    uint32_t snap_w_ = 0, snap_h_ = 0;
    reshade::api::format snap_format_ = reshade::api::format::unknown; // the frame's format

    // The native picture (8-bit RGBA): drawn by the capture preset through native_rtv_,
    // read by the user's preset through native_srv_.
    reshade::api::resource native_ = {};
    reshade::api::resource_view native_rtv_ = {}, native_srv_ = {};
    uint32_t native_w_ = 0, native_h_ = 0;

    // The user's preset's output, the size of the grid's rectangle and in the frame's
    // format family, so it can be copied into the frame.
    reshade::api::resource out_ = {};
    reshade::api::resource_view out_rtv_ = {}, out_unused_ = {};
    uint32_t out_w_ = 0, out_h_ = 0;
    reshade::api::format out_format_ = reshade::api::format::unknown; // the frame format it was made for
};
