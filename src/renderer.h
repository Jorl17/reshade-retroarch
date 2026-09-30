#pragma once
// The offline renderer (Direct3D 11): takes a frame, rebuilds the game's original
// low-resolution picture from it, runs a RetroArch shader preset on that picture and
// writes the result back into the frame.
//
// Used by the offline tool tools/render_png.cpp, which applies it to a PNG screenshot. The
// add-on itself uses FrameRenderer (frame_renderer.h), which does the same steps through
// ReShade's API.

#include "chain.h"
#include "grid_detect.h"

#include <d3d11.h>
#include <cstdint>
#include <string>

// Turns a game frame into a shader-processed frame. Each frame the caller does:
//  1. snapshot(): copy the frame into a texture owned by the renderer, because step 2
//     writes into the frame itself.
//  2. render(): rebuild the "native image" from the copy, run the shader preset on it,
//     and copy the result into the frame.
// The native image is the picture as the game originally drew it. Many games draw pixel
// art at a low resolution (for example 424x240) and stretch it to fill the screen, so
// each original pixel becomes a block of screen pixels. A PixelGrid (grid_detect.h) gives
// that original resolution and the rectangle of the frame the stretched picture covers;
// render() reads one frame pixel from the centre of each block and gets the picture
// back at its original size, which is what RetroArch shaders are written for. That step
// is itself a one-pass RetroArch preset, the capture preset (capture.h, capture.slang),
// run through librashader like the user's preset.
//
// Owns the textures and shaders this needs and recreates textures when sizes or formats
// change. Not thread-safe: calls must not overlap.
class Renderer
{
public:
    Renderer() = default;
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    ~Renderer() { shutdown(); }

    // Prepares the renderer to draw with `device`: loads the capture preset (capture.h).
    // librashader must already be loaded (librashader_api.h). Releases anything from an
    // earlier init() first. Returns false and sets `error` on failure. Must succeed
    // before snapshot() or render() is called.
    bool init(ID3D11Device *device, std::string &error);

    // Releases every GPU object the renderer holds, returning it to its state before
    // init(). Safe to call more than once; the destructor calls it.
    void shutdown();

    // Copies `frame` (the game's finished picture for this frame, normally the back
    // buffer: the texture that will be shown on screen) into a texture owned by the
    // renderer, and returns that copy. render() reads the copy, which lets it write its
    // result into `frame` itself. The copy always has one sample per pixel, even when the
    // frame is multisampled (MSAA).
    // Call it every frame before render(), and before anything else draws over the frame.
    // Returns nullptr and sets `error` when the frame's format is not supported (see
    // supported_format()) or the copy cannot be created.
    ID3D11Texture2D *snapshot(ID3D11DeviceContext *ctx, ID3D11Texture2D *frame, std::string &error);

    // Draws the shader-processed picture into `dst`, normally the same texture that was
    // passed to snapshot(). Rebuilds the native image described by `grid` from the last
    // snapshot (grid.native_w x grid.native_h pixels, taken from grid's rectangle), runs
    // `chain` on it, and copies the result into `dst` over grid's rectangle, at the
    // rectangle's size. `frame_count` is the frame number passed to the shaders; animated
    // effects (noise, flicker) use it, so it should go up by one each frame.
    // Returns false and sets `error` when there is no snapshot, `grid` is invalid or
    // reaches outside the snapshot, `dst` is multisampled, or a texture or the chain fails.
    //
    // Pixels of `dst` outside the rectangle (black bars, HD border art) are never touched.
    // That is why the chain draws into a separate texture the size of the rectangle,
    // which is then copied into place: librashader clears its whole output texture, so
    // drawing straight into `dst` would erase them.
    //
    // Leaves its own shaders, render target and viewport bound on `ctx`; it does not
    // restore what the caller had bound.
    bool render(ID3D11DeviceContext *ctx, const PixelGrid &grid, ShaderChain &chain, ID3D11Texture2D *dst,
                uint64_t frame_count, std::string &error);

    // The native image rebuilt by the last render() (grid.native_w x grid.native_h, 8 bits
    // per channel RGBA), or null before the first one. tools/render_png.cpp saves it.
    ID3D11Texture2D *native_texture() const { return native_tex_; }

    // Returns true for the frame formats the capture can read: 8 bits per channel
    // RGBA, BGRA or BGRX (plain, sRGB or typeless) and 10-bit RGB with 2-bit alpha.
    // Returns false for everything else, notably the 16-bit float formats used for HDR,
    // whose linear or scRGB values RetroArch shaders are not written for. (A 10-bit
    // frame can also hold HDR10 content; the add-on detects that separately, from the
    // swap chain's colour space.)
    static bool supported_format(DXGI_FORMAT f);

private:
    // Makes sure native_tex_ exists and is `w` x `h`, recreating it (and its views) when
    // the size changed. Returns false and sets `error` if it cannot be created.
    bool ensure_native(int w, int h, std::string &error);
    // Makes sure out_tex_ exists, is `w` x `h`, and belongs to the same format family as
    // `format` (the format of the texture it will be copied into), recreating it when
    // either changed. Returns false and sets `error` if it cannot be created.
    bool ensure_output(int w, int h, DXGI_FORMAT format, std::string &error);
    // Passes the grid's rectangle and native size to the capture preset as its parameters,
    // when they differ from the last ones passed. Returns false and sets `error` on failure.
    bool set_capture_grid(const PixelGrid &grid, std::string &error);

    // Device passed to init(). Not owned: the renderer holds no reference to it.
    ID3D11Device *device_ = nullptr;
    // The capture preset (capture.slangp), compiled for `device_`, and the grid whose
    // values were last passed to it (see set_capture_grid).
    ShaderChain capture_;
    PixelGrid capture_grid_;

    // The snapshot: a copy of the frame. Created "typeless" (bytes per pixel fixed, but
    // their meaning, plain or sRGB, left to each view) so snap_srv_ can read it as plain
    // values even when the frame is sRGB. snap_srv_ (a shader resource view: how shaders
    // read a texture) is what the capture preset reads. snap_desc_ describes the copy; when
    // the next frame's description differs, the copy is recreated.
    ID3D11Texture2D *snap_tex_ = nullptr;
    ID3D11ShaderResourceView *snap_srv_ = nullptr;
    D3D11_TEXTURE2D_DESC snap_desc_ = {};
    DXGI_FORMAT snap_format_ = DXGI_FORMAT_UNKNOWN; // the frame's format (the texture is typeless)

    // The rebuilt native image. The capture preset draws into it through native_rtv_ (a
    // render target view: how the GPU draws into a texture); the shader chain reads it
    // through native_srv_. native_w_ x native_h_ is its size.
    ID3D11Texture2D *native_tex_ = nullptr;
    ID3D11RenderTargetView *native_rtv_ = nullptr;
    ID3D11ShaderResourceView *native_srv_ = nullptr;
    int native_w_ = 0, native_h_ = 0;

    // The shader chain's output, the size of the grid's rectangle, in the frame's format
    // family so it can be copied into the frame. librashader draws into it through
    // out_rtv_. out_desc_ describes it; out_format_ is the frame format it was made for.
    ID3D11Texture2D *out_tex_ = nullptr;
    ID3D11RenderTargetView *out_rtv_ = nullptr;
    D3D11_TEXTURE2D_DESC out_desc_ = {};
    DXGI_FORMAT out_format_ = DXGI_FORMAT_UNKNOWN;
};
