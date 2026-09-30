#pragma once
// Loads and runs RetroArch shader presets through librashader, a library that runs
// RetroArch's "slang" shaders outside RetroArch. A preset is a .slangp file: a list of
// shader passes (each a .slang shader file) applied one after another, plus values for
// the shaders' adjustable parameters. This file wraps librashader's compiled preset (a
// "filter chain") for each graphics API in one class, and reads and compares preset
// parameters, which the add-on's window shows as sliders and saves.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// One adjustable parameter of a preset, shown as a slider in the add-on's window.
// Shaders declare parameters with `#pragma parameter` lines; a preset can set their
// values.
struct ShaderParam
{
    // `name` identifies the parameter in .slangp files and librashader calls;
    // `description` is the label to show (the name when the shader has none).
    std::string name, description;
    float value = 0;   // value in use now (ShaderChain::set_param changes it)
    float initial = 0; // value in the preset: set in the .slangp, or the shader's default
    // Slider range and step, as declared by the shader.
    float minimum = 0, maximum = 0, step = 0;
};

// Fills `out` with the parameters of the preset file at `preset_path` (a UTF-8 path),
// each with its value in the preset. Reads files only: nothing is compiled and
// no GPU is needed. Returns false and sets `error` if librashader is not loaded (see
// librashader_api.h) or the preset cannot be read.
bool read_preset_params(const std::string &preset_path, std::vector<ShaderParam> &out, std::string &error);

// Returns the parameters in `current` whose value differs from their value in `base`, as
// (name, value) pairs in `current`'s order; a parameter missing from `base` is
// always included, and values within 1e-6 count as equal.
// This is what a companion .slangp has to store. (A companion is the small .slangp the
// add-on writes next to a ReShade preset: a #reference line with the path of the preset
// the user picked, which is `base`, plus the parameters the user changed; see companion.h.)
// `current` normally comes from the chain compiled from the companion itself, so values
// saved there earlier still differ from `base` and are kept, and values moved back to
// the base value drop out.
std::vector<std::pair<std::string, float>> param_overrides(const std::vector<ShaderParam> &current,
                                                           const std::vector<ShaderParam> &base);

// The graphics APIs a ShaderChain can be compiled for (librashader has a runtime for each).
enum class GraphicsApi
{
    d3d9,
    d3d11,
    d3d12,
    opengl,
    vulkan,
};

// Native objects of the device a chain is compiled for.
struct ChainDevice
{
    GraphicsApi api = GraphicsApi::d3d11;
    // ID3D11Device*, ID3D12Device*, IDirect3DDevice9* or VkDevice (as an integer).
    uint64_t device = 0;
    // Vulkan only (see vulkan_support.h): the VkInstance and VkPhysicalDevice, the VkQueue
    // the chain's work is submitted to and its queue family, and vkGetInstanceProcAddr and
    // vkGetDeviceProcAddr.
    uint64_t instance = 0, physical_device = 0, queue = 0;
    uint32_t queue_family = 0;
    void *get_instance_proc_addr = nullptr, *get_device_proc_addr = nullptr;
};

// How many frames librashader's Direct3D 12 and Vulkan runtimes keep per-frame objects for:
// what a chain draws in one frame() call is reused or freed by the frame() call this many
// calls later, so the GPU must have finished the earlier frame by then. Chains are created
// with this value, and the add-on's renderer (frame_renderer.h) waits for the GPU to finish
// the frame this many frames back before recording a new one, so it holds however many
// frames the GPU is behind.
constexpr uint32_t kFramesInFlight = 3;

// One image a chain reads (its input) or draws into (its output), as native handles of
// the chain's API. Which fields are used depends on the API:
//  - Direct3D 11: `view` only: an ID3D11ShaderResourceView* for the input, an
//    ID3D11RenderTargetView* for the output.
//  - Direct3D 12: `resource` (an ID3D12Resource*), `view` (the CPU descriptor handle of a
//    shader resource view for the input, of a render target view for the output), and,
//    for the output, `format` (a DXGI_FORMAT), `width` and `height`. The input must be in
//    a pixel-shader-resource state and the output in the render-target state.
//  - OpenGL: `resource` (the texture's name, a GLuint), `format` (its sized internal
//    format, e.g. GL_RGBA8), `width` and `height`, for input and output alike.
//  - Direct3D 9: `resource` for the input (an IDirect3DTexture9*), `view` for the output
//    (the IDirect3DSurface9* to draw into).
//  - Vulkan: `resource` (a VkImage), `format` (a VkFormat), `width` and `height`. The
//    input must be in the SHADER_READ_ONLY_OPTIMAL layout and the output in
//    COLOR_ATTACHMENT_OPTIMAL, where it stays.
struct ChainImage
{
    uint64_t resource = 0;          // the texture or image itself
    uint64_t view = 0;              // a view of it, for APIs that take views
    uint32_t format = 0;            // its native format, for APIs that need it
    uint32_t width = 0, height = 0; // its size in pixels
};

// A RetroArch preset compiled by librashader for one device, plus its parameters.
// create() loads and compiles a preset, frame() draws it, set_param() changes a parameter
// while it runs. Needs librashader loaded first (libra::load in librashader_api.h). The
// destructor frees the compiled preset.
class ShaderChain
{
public:
    ShaderChain() = default;
    ShaderChain(const ShaderChain &) = delete;
    ShaderChain &operator=(const ShaderChain &) = delete;
    ~ShaderChain() { destroy(); }

    // True for the APIs create() can compile for. For the others it fails with an error
    // message.
    static bool supports(GraphicsApi api);

    // Loads the preset file at `preset_path` (a UTF-8 path) and compiles all its shader
    // passes for `device`. Any preset loaded before is freed first, even if this one then
    // fails. Returns false and sets `error` if librashader is not loaded, the API is not
    // supported, or the preset fails to load or compile; ready() is then false. Compiling
    // runs on the calling thread and can take a while for large presets.
    bool create(const ChainDevice &device, const std::string &preset_path, std::string &error);
    // Frees the compiled preset and clears its parameters and path. Safe to call when
    // nothing is loaded.
    void destroy();
    // True when a preset is compiled and frame() can draw.
    bool ready() const { return chain_ != nullptr; }

    // Draws one frame of the preset: reads `input` (the image to process; in this project,
    // the game's native image) and draws the result into the area (x, y, w, h) of
    // `output`. `frame_count` is the frame number passed to the shaders; animated effects
    // (noise, flicker, interlacing) use it, so it should go up by one each frame. The work
    // is recorded on `commands`, the API's native command recorder: an
    // ID3D11DeviceContext* on Direct3D 11, an open ID3D12GraphicsCommandList* on
    // Direct3D 12 (librashader binds its own descriptor heaps on it), a VkCommandBuffer in
    // the recording state on Vulkan; unused on OpenGL and Direct3D 9. Direct3D 9 draws on
    // the device given to create(); OpenGL uses the context current on the calling thread
    // (the one the chain was created with). Returns false and sets `error` if no preset is
    // loaded or librashader returns an error.
    // Do not rely on the rest of `output` being kept: librashader clears all of it.
    bool frame(uint64_t commands, const ChainImage &input, const ChainImage &output, int x, int y, int w, int h,
               uint64_t frame_count, std::string &error);

    // Sets parameter `name` of the loaded preset to `value`, from the next frame() on,
    // and updates it in params(). Returns false if no preset is loaded (leaving `error`
    // unchanged), or if librashader returns an error for the change, for example for an
    // unknown name (with `error` set).
    bool set_param(const std::string &name, float value, std::string &error);
    // The loaded preset's parameters with their current values; empty when nothing is
    // loaded.
    const std::vector<ShaderParam> &params() const { return params_; }
    // The path given to the create() call that succeeded; empty when nothing is loaded.
    const std::string &preset() const { return preset_; }

private:
    GraphicsApi api_ = GraphicsApi::d3d11;
    // librashader's handle to the compiled preset for api_ (a libra_d3d11_filter_chain_t
    // and so on), or null. Stored as void* so this header does not need librashader's
    // headers.
    void *chain_ = nullptr;
    std::vector<ShaderParam> params_;
    std::string preset_;
};
