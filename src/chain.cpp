// Implementation of chain.h: the calls into librashader that load RetroArch presets,
// read their parameters, and compile and run them on each graphics API. librashader's
// functions are reached through libra::api() (librashader_api.h), a table of function
// pointers filled when the DLL is loaded at runtime.

#include "chain.h"
#include "librashader_api.h"
#include "vulkan_support.h"

#include <cmath>

namespace
{
// Fills `out` with the parameters of `preset` (a preset already loaded with
// librashader's preset_create), each with the value the preset gives it: the value
// set in the .slangp if any, otherwise the shader's default. Returns false and sets
// `error` if librashader cannot list them.
// A .slangp can start with `#reference "other.slangp"` to build on another preset and
// then set some values itself, and the referenced file can do the same. When several
// files in that chain set a parameter, the value read last wins (the referencing file's
// own lines come after its #reference), as when the preset runs.
// (libra_preset_get_param is not used: it returns the first value set, and nothing at
// all for parameters the preset does not set.)
bool read_params(libra_shader_preset_t &preset, std::vector<ShaderParam> &out, std::string &error)
{
    const libra_instance_t &api = libra::api();
    out.clear();
    libra_preset_param_list_t list = {};
    if (libra_error_t err = api.preset_get_runtime_params(&preset, &list))
    {
        error = libra::take_error(err);
        return false;
    }
    for (uint64_t i = 0; i < list.length; ++i)
    {
        const libra_preset_param_t &p = list.parameters[i];
        ShaderParam sp;
        sp.name = p.name ? p.name : "";
        sp.description = p.description ? p.description : sp.name;
        sp.initial = p.initial;
        sp.value = p.initial;
        sp.minimum = p.minimum;
        sp.maximum = p.maximum;
        sp.step = p.step;
        out.push_back(sp);
    }
    api.preset_free_runtime_params(list);
    return true;
}
} // namespace

// Loads the preset (without compiling it), reads its parameters into `out` and frees
// it. Returns false and sets `error` on failure.
bool read_preset_params(const std::string &preset_path, std::vector<ShaderParam> &out, std::string &error)
{
    if (!libra::loaded())
    {
        error = "librashader is not loaded";
        return false;
    }
    const libra_instance_t &api = libra::api();
    libra_shader_preset_t preset = nullptr;
    if (libra_error_t err = api.preset_create(preset_path.c_str(), &preset))
    {
        error = libra::take_error(err);
        return false;
    }
    const bool ok = read_params(preset, out, error);
    api.preset_free(&preset);
    return ok;
}

// Returns the (name, value) pairs of `current` that differ from `base` (see chain.h).
std::vector<std::pair<std::string, float>> param_overrides(const std::vector<ShaderParam> &current,
                                                           const std::vector<ShaderParam> &base)
{
    std::vector<std::pair<std::string, float>> out;
    for (const ShaderParam &p : current)
    {
        // Find the parameter with the same name in `base`, if any.
        const ShaderParam *b = nullptr;
        for (const ShaderParam &x : base)
            if (x.name == p.name)
                b = &x;
        if (b == nullptr || std::fabs(p.value - b->initial) > 1e-6f)
            out.emplace_back(p.name, p.value);
    }
    return out;
}

bool ShaderChain::supports(GraphicsApi api)
{
    return api == GraphicsApi::d3d9 || api == GraphicsApi::d3d11 || api == GraphicsApi::d3d12 ||
           api == GraphicsApi::opengl || api == GraphicsApi::vulkan;
}

// Finds an OpenGL function for librashader: wglGetProcAddress returns the functions added
// after OpenGL 1.1, GetProcAddress on opengl32.dll the 1.1 ones (wglGetProcAddress
// returns small non-null values for some failures, which count as "not found").
// opengl32.dll is the one the game has loaded; it is looked up at run time so the add-on
// does not load OpenGL into games that use Direct3D.
static const void *gl_function(const char *name)
{
    const HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (gl == nullptr)
        return nullptr;
    using GetProc = PROC(WINAPI *)(LPCSTR);
    const auto wgl_get_proc = reinterpret_cast<GetProc>(reinterpret_cast<void *>(GetProcAddress(gl, "wglGetProcAddress")));
    if (wgl_get_proc != nullptr)
    {
        const PROC p = wgl_get_proc(name);
        const auto v = reinterpret_cast<intptr_t>(p);
        if (v != 0 && v != 1 && v != 2 && v != 3 && v != -1)
            return reinterpret_cast<const void *>(p);
    }
    return reinterpret_cast<const void *>(GetProcAddress(gl, name));
}

// Fills the per-frame values librashader passes to the shaders, the same for every API:
// playing forwards (frame_direction 1; -1 would mean rewinding), one shader run per
// displayed frame (subframe 1 of 1), a source frame rate of 60 per second (assumed; the
// real rate is not known here), and HDR brightness at its documented default (only HDR
// presets read it).
template <typename Options>
static Options frame_options()
{
    Options opt = {};
    opt.version = LIBRASHADER_CURRENT_VERSION;
    opt.frame_direction = 1;
    opt.total_subframes = 1;
    opt.current_subframe = 1;
    opt.frames_per_second = 60.0f;
    opt.brightness_nits = 200.0f;
    return opt;
}

// Human-readable name of `api`, for messages.
static const char *api_name(GraphicsApi api)
{
    switch (api)
    {
    case GraphicsApi::d3d9:
        return "Direct3D 9";
    case GraphicsApi::d3d11:
        return "Direct3D 11";
    case GraphicsApi::d3d12:
        return "Direct3D 12";
    case GraphicsApi::opengl:
        return "OpenGL";
    case GraphicsApi::vulkan:
        return "Vulkan";
    }
    return "this graphics API";
}

// Loads `preset_path`, reads its parameters and compiles it for `device`. Returns false
// and sets `error` on failure, leaving the chain empty.
bool ShaderChain::create(const ChainDevice &device, const std::string &preset_path, std::string &error)
{
    destroy();
    if (!libra::loaded())
    {
        error = "librashader is not loaded";
        return false;
    }
    if (!supports(device.api))
    {
        error = std::string(api_name(device.api)) + " is not supported yet";
        return false;
    }
    const libra_instance_t &api = libra::api();

    // Parse the preset file and the files it references.
    libra_shader_preset_t preset = nullptr;
    if (libra_error_t err = api.preset_create(preset_path.c_str(), &preset))
    {
        error = libra::take_error(err);
        return false;
    }

    // Read the parameters now: compiling below uses up the preset object.
    if (!read_params(preset, params_, error))
    {
        api.preset_free(&preset);
        return false;
    }

    // Compile every pass for the device. The call takes ownership of `preset` and sets it
    // to null, so it is freed here only if librashader did not take it.
    void *chain = nullptr;
    libra_error_t err = nullptr;
    switch (device.api)
    {
    case GraphicsApi::d3d11:
    {
        filter_chain_d3d11_opt_t opt = {};
        opt.version = LIBRASHADER_CURRENT_VERSION;
        libra_d3d11_filter_chain_t c = nullptr;
        err = api.d3d11_filter_chain_create(&preset, reinterpret_cast<ID3D11Device *>(device.device), &opt, &c);
        chain = c;
        break;
    }
    case GraphicsApi::d3d12:
    {
        filter_chain_d3d12_opt_t opt = {};
        opt.version = LIBRASHADER_CURRENT_VERSION;
        opt.frames_in_flight = kFramesInFlight;
        libra_d3d12_filter_chain_t c = nullptr;
        err = api.d3d12_filter_chain_create(&preset, reinterpret_cast<ID3D12Device *>(device.device), &opt, &c);
        chain = c;
        break;
    }
    case GraphicsApi::vulkan:
    {
        libra_device_vk_t vk = {};
        vk.physical_device = reinterpret_cast<VkPhysicalDevice>(device.physical_device);
        vk.instance = reinterpret_cast<VkInstance>(device.instance);
        vk.device = reinterpret_cast<VkDevice>(device.device);
        vk.queue = reinterpret_cast<VkQueue>(device.queue);
        vk.entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(device.get_instance_proc_addr);
        filter_chain_vk_opt_t opt = {};
        opt.version = LIBRASHADER_CURRENT_VERSION;
        opt.frames_in_flight = kFramesInFlight;
        // The "deferred" create records the chain's GPU setup (uploading its look-up
        // textures) into a command buffer given here, run on the game's queue family.
        // (librashader's plain create makes its own command pool, always for queue family
        // 0, which is not necessarily the family of the game's queue.)
        VulkanFunctions vkf;
        if (!vkf.load(vk.device, device.get_device_proc_addr))
        {
            api.preset_free(&preset);
            error = "Vulkan functions could not be loaded";
            params_.clear();
            return false;
        }
        libra_vk_filter_chain_t c = nullptr;
        std::string setup_error;
        const bool setup_ok = vulkan_run_once(
            vkf, vk.device, vk.queue, device.queue_family,
            [&](VkCommandBuffer cmd) {
                err = api.vk_filter_chain_create_deferred(&preset, vk, cmd, &opt, &c);
                if (err != nullptr)
                    setup_error = "librashader could not create the chain";
                return err == nullptr;
            },
            setup_error);
        if (!setup_ok && err == nullptr)
        {
            // The chain was made but its setup never ran on the GPU: it cannot be used.
            if (c != nullptr)
                api.vk_filter_chain_free(&c);
            if (preset != nullptr)
                api.preset_free(&preset);
            error = setup_error;
            params_.clear();
            return false;
        }
        chain = c;
        break;
    }
    case GraphicsApi::d3d9:
    {
        filter_chain_d3d9_opt_t opt = {};
        opt.version = LIBRASHADER_CURRENT_VERSION;
        libra_d3d9_filter_chain_t c = nullptr;
        err = api.d3d9_filter_chain_create(&preset, reinterpret_cast<IDirect3DDevice9 *>(device.device), &opt, &c);
        chain = c;
        break;
    }
    case GraphicsApi::opengl:
    {
        // Needs the OpenGL context to be current on this thread.
        filter_chain_gl_opt_t opt = {};
        opt.version = LIBRASHADER_CURRENT_VERSION;
        libra_gl_filter_chain_t c = nullptr;
        err = api.gl_filter_chain_create(&preset, gl_function, &opt, &c);
        chain = c;
        break;
    }
    default:
        break;
    }
    if (preset != nullptr)
        api.preset_free(&preset);
    if (err != nullptr)
    {
        error = libra::take_error(err);
        params_.clear();
        return false;
    }
    api_ = device.api;
    chain_ = chain;
    preset_ = preset_path;
    return true;
}

// Frees the compiled preset, if any, and clears the parameters and path.
void ShaderChain::destroy()
{
    if (chain_ != nullptr)
    {
        switch (api_)
        {
        case GraphicsApi::d3d11:
        {
            libra_d3d11_filter_chain_t chain = static_cast<libra_d3d11_filter_chain_t>(chain_);
            libra::api().d3d11_filter_chain_free(&chain);
            break;
        }
        case GraphicsApi::d3d12:
        {
            libra_d3d12_filter_chain_t chain = static_cast<libra_d3d12_filter_chain_t>(chain_);
            libra::api().d3d12_filter_chain_free(&chain);
            break;
        }
        case GraphicsApi::opengl:
        {
            libra_gl_filter_chain_t chain = static_cast<libra_gl_filter_chain_t>(chain_);
            libra::api().gl_filter_chain_free(&chain);
            break;
        }
        case GraphicsApi::d3d9:
        {
            libra_d3d9_filter_chain_t chain = static_cast<libra_d3d9_filter_chain_t>(chain_);
            libra::api().d3d9_filter_chain_free(&chain);
            break;
        }
        case GraphicsApi::vulkan:
        {
            libra_vk_filter_chain_t chain = static_cast<libra_vk_filter_chain_t>(chain_);
            libra::api().vk_filter_chain_free(&chain);
            break;
        }
        default:
            break;
        }
        chain_ = nullptr;
    }
    params_.clear();
    preset_.clear();
}

// Runs the compiled preset once from `input` into the area (x, y, w, h) of `output`.
// Returns false and sets `error` if nothing is loaded or librashader fails.
bool ShaderChain::frame(uint64_t commands, const ChainImage &input, const ChainImage &output, int x, int y, int w,
                        int h, uint64_t frame_count, std::string &error)
{
    if (chain_ == nullptr)
    {
        error = "no shader loaded";
        return false;
    }
    // The area of `output` to draw into.
    libra_viewport_t viewport = {float(x), float(y), uint32_t(w), uint32_t(h)};

    libra_error_t err = nullptr;
    switch (api_)
    {
    case GraphicsApi::d3d11:
    {
        const frame_d3d11_opt_t opt = frame_options<frame_d3d11_opt_t>();
        // No transform matrix is passed (nullptr), so librashader uses its default one.
        libra_d3d11_filter_chain_t chain = static_cast<libra_d3d11_filter_chain_t>(chain_);
        err = libra::api().d3d11_filter_chain_frame(
            &chain, reinterpret_cast<ID3D11DeviceContext *>(commands), size_t(frame_count),
            reinterpret_cast<ID3D11ShaderResourceView *>(input.view),
            reinterpret_cast<ID3D11RenderTargetView *>(output.view), &viewport, nullptr, &opt);
        break;
    }
    case GraphicsApi::d3d12:
    {
        const frame_d3d12_opt_t opt = frame_options<frame_d3d12_opt_t>();
        // Both images come with our own descriptors (views), so librashader does not have
        // to create views of the resources itself (it could not for the typeless snapshot).
        libra_image_d3d12_t in = {}, out = {};
        in.image_type = LIBRA_D3D12_IMAGE_TYPE_SOURCE_IMAGE;
        in.handle.source.descriptor.ptr = SIZE_T(input.view);
        in.handle.source.resource = reinterpret_cast<ID3D12Resource *>(input.resource);
        out.image_type = LIBRA_D3D12_IMAGE_TYPE_OUTPUT_IMAGE;
        out.handle.output.descriptor.ptr = SIZE_T(output.view);
        out.handle.output.format = DXGI_FORMAT(output.format);
        out.handle.output.width = output.width;
        out.handle.output.height = output.height;
        libra_d3d12_filter_chain_t chain = static_cast<libra_d3d12_filter_chain_t>(chain_);
        err = libra::api().d3d12_filter_chain_frame(&chain, reinterpret_cast<ID3D12GraphicsCommandList *>(commands),
                                                    size_t(frame_count), in, out, &viewport, nullptr, &opt);
        break;
    }
    case GraphicsApi::opengl:
    {
        const frame_gl_opt_t opt = frame_options<frame_gl_opt_t>();
        const libra_image_gl_t in = {uint32_t(input.resource), input.format, input.width, input.height};
        const libra_image_gl_t out = {uint32_t(output.resource), output.format, output.width, output.height};
        libra_gl_filter_chain_t chain = static_cast<libra_gl_filter_chain_t>(chain_);
        err = libra::api().gl_filter_chain_frame(&chain, size_t(frame_count), in, out, &viewport, nullptr, &opt);
        break;
    }
    case GraphicsApi::vulkan:
    {
        const frame_vk_opt_t opt = frame_options<frame_vk_opt_t>();
        const libra_image_vk_t in = {reinterpret_cast<VkImage>(input.resource), VkFormat(input.format), input.width,
                                     input.height};
        const libra_image_vk_t out = {reinterpret_cast<VkImage>(output.resource), VkFormat(output.format),
                                      output.width, output.height};
        libra_vk_filter_chain_t chain = static_cast<libra_vk_filter_chain_t>(chain_);
        err = libra::api().vk_filter_chain_frame(&chain, reinterpret_cast<VkCommandBuffer>(commands),
                                                 size_t(frame_count), in, out, &viewport, nullptr, &opt);
        break;
    }
    case GraphicsApi::d3d9:
    {
        const frame_d3d9_opt_t opt = frame_options<frame_d3d9_opt_t>();
        libra_d3d9_filter_chain_t chain = static_cast<libra_d3d9_filter_chain_t>(chain_);
        err = libra::api().d3d9_filter_chain_frame(&chain, size_t(frame_count),
                                                   reinterpret_cast<IDirect3DTexture9 *>(input.resource),
                                                   reinterpret_cast<IDirect3DSurface9 *>(output.view), &viewport,
                                                   nullptr, &opt);
        break;
    }
    default:
        error = std::string(api_name(api_)) + " is not supported yet";
        return false;
    }
    if (err != nullptr)
    {
        error = libra::take_error(err);
        return false;
    }
    return true;
}

// Changes parameter `name` to `value` in the compiled preset and in params_. Returns
// false (without setting `error`) when nothing is loaded, or false with `error` set
// when librashader rejects the change.
bool ShaderChain::set_param(const std::string &name, float value, std::string &error)
{
    if (chain_ == nullptr)
        return false;
    libra_error_t err = nullptr;
    switch (api_)
    {
    case GraphicsApi::d3d11:
    {
        libra_d3d11_filter_chain_t chain = static_cast<libra_d3d11_filter_chain_t>(chain_);
        err = libra::api().d3d11_filter_chain_set_param(&chain, name.c_str(), value);
        break;
    }
    case GraphicsApi::d3d12:
    {
        libra_d3d12_filter_chain_t chain = static_cast<libra_d3d12_filter_chain_t>(chain_);
        err = libra::api().d3d12_filter_chain_set_param(&chain, name.c_str(), value);
        break;
    }
    case GraphicsApi::opengl:
    {
        libra_gl_filter_chain_t chain = static_cast<libra_gl_filter_chain_t>(chain_);
        err = libra::api().gl_filter_chain_set_param(&chain, name.c_str(), value);
        break;
    }
    case GraphicsApi::d3d9:
    {
        libra_d3d9_filter_chain_t chain = static_cast<libra_d3d9_filter_chain_t>(chain_);
        err = libra::api().d3d9_filter_chain_set_param(&chain, name.c_str(), value);
        break;
    }
    case GraphicsApi::vulkan:
    {
        libra_vk_filter_chain_t chain = static_cast<libra_vk_filter_chain_t>(chain_);
        err = libra::api().vk_filter_chain_set_param(&chain, name.c_str(), value);
        break;
    }
    default:
        return false;
    }
    if (err != nullptr)
    {
        error = libra::take_error(err);
        return false;
    }
    for (ShaderParam &p : params_)
        if (p.name == name)
            p.value = value;
    return true;
}
