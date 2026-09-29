// Vulkan backend of the test host (see backend.h): a Vulkan swap chain on the window.
// Pictures are uploaded once into CPU-visible buffers and copied into the swap chain
// image with vkCmdCopyBufferToImage.
#include "backend.h"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <map>

namespace
{
// The Vulkan functions the host uses. Vulkan is reached through the system loader,
// vulkan-1.dll, which every Vulkan driver installs; it is loaded at run time and each
// function is looked up by name, so building needs only the headers, not the Vulkan SDK.
struct Vk
{
#define VK_FN(name) PFN_##name name = nullptr;
    VK_FN(vkGetInstanceProcAddr)
    VK_FN(vkCreateInstance)
    VK_FN(vkDestroyInstance)
    VK_FN(vkEnumeratePhysicalDevices)
    VK_FN(vkGetPhysicalDeviceQueueFamilyProperties)
    VK_FN(vkGetPhysicalDeviceMemoryProperties)
    VK_FN(vkGetPhysicalDeviceSurfaceSupportKHR)
    VK_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
    VK_FN(vkGetPhysicalDeviceSurfaceFormatsKHR)
    VK_FN(vkGetPhysicalDeviceSurfacePresentModesKHR)
    VK_FN(vkCreateWin32SurfaceKHR)
    VK_FN(vkDestroySurfaceKHR)
    VK_FN(vkCreateDevice)
    VK_FN(vkGetDeviceProcAddr)
    VK_FN(vkDestroyDevice)
    VK_FN(vkGetDeviceQueue)
    VK_FN(vkCreateSwapchainKHR)
    VK_FN(vkDestroySwapchainKHR)
    VK_FN(vkGetSwapchainImagesKHR)
    VK_FN(vkAcquireNextImageKHR)
    VK_FN(vkQueuePresentKHR)
    VK_FN(vkCreateCommandPool)
    VK_FN(vkDestroyCommandPool)
    VK_FN(vkAllocateCommandBuffers)
    VK_FN(vkBeginCommandBuffer)
    VK_FN(vkEndCommandBuffer)
    VK_FN(vkCmdPipelineBarrier)
    VK_FN(vkCmdCopyBufferToImage)
    VK_FN(vkCmdCopyImageToBuffer)
    VK_FN(vkCmdClearColorImage)
    VK_FN(vkQueueSubmit)
    VK_FN(vkQueueWaitIdle)
    VK_FN(vkDeviceWaitIdle)
    VK_FN(vkCreateBuffer)
    VK_FN(vkDestroyBuffer)
    VK_FN(vkGetBufferMemoryRequirements)
    VK_FN(vkAllocateMemory)
    VK_FN(vkFreeMemory)
    VK_FN(vkBindBufferMemory)
    VK_FN(vkMapMemory)
    VK_FN(vkUnmapMemory)
    VK_FN(vkCreateFence)
    VK_FN(vkDestroyFence)
    VK_FN(vkWaitForFences)
    VK_FN(vkResetFences)
#undef VK_FN
};

// A buffer and the memory bound to it.
struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

// Every operation records the one command buffer, submits it and waits until the GPU has
// finished: slow, but simple, and speed does not matter in a test host. Swap chain
// images are kept in the PRESENT_SRC layout between operations. On Windows the swap
// chain must be the size of the window's client area, so the window is sized to the
// requested back buffer (see size_window in backend.h). See Backend in backend.h for
// what each public function does.
class Vulkan : public Backend
{
public:
    ~Vulkan() override
    {
        if (device_ != VK_NULL_HANDLE)
        {
            vk_.vkDeviceWaitIdle(device_);
            for (auto &[img, b] : uploads_)
                destroy(b);
            if (fence_ != VK_NULL_HANDLE)
                vk_.vkDestroyFence(device_, fence_, nullptr);
            if (pool_ != VK_NULL_HANDLE)
                vk_.vkDestroyCommandPool(device_, pool_, nullptr);
            if (swapchain_ != VK_NULL_HANDLE)
                vk_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            vk_.vkDestroyDevice(device_, nullptr);
        }
        if (surface_ != VK_NULL_HANDLE)
            vk_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_ != VK_NULL_HANDLE)
            vk_.vkDestroyInstance(instance_, nullptr);
        if (lib_ != nullptr)
            FreeLibrary(lib_);
    }

    bool init(const Options &o, std::string &error, bool &unsupported) override
    {
        o_ = o;
        if (!create_instance(error, unsupported) || !create_device(error))
            return false;
        if (!pick_format(error, unsupported))
            return false;
        if (!size_window(o_.hwnd, o.width, o.height))
        {
            error = "could not size the window";
            return false;
        }
        return create_swapchain(o.width, o.height, error, unsupported);
    }

    bool resize(UINT w, UINT h, std::string &error) override
    {
        bool unsupported = false;
        if (!size_window(o_.hwnd, w, h))
        {
            error = "could not resize the window";
            return false;
        }
        vk_.vkDeviceWaitIdle(device_);
        return create_swapchain(w, h, error, unsupported);
    }

    bool draw(const Image *img, std::string &error) override
    {
        vk_.vkResetFences(device_, 1, &fence_);
        if (vk_.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, VK_NULL_HANDLE, fence_, &current_) < 0 ||
            vk_.vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        {
            error = "vkAcquireNextImageKHR failed";
            return false;
        }
        const VkImage image = images_[current_];
        const Buffer *up = img != nullptr ? upload(*img) : nullptr;
        if (img != nullptr && up == nullptr)
        {
            error = "could not create the picture upload buffer";
            return false;
        }
        begin();
        barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (up != nullptr)
        {
            const VkBufferImageCopy region = copy_region(img->w, img->h);
            vk_.vkCmdCopyBufferToImage(cmd_, up->buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        else
        {
            const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
            const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk_.vkCmdClearColorImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        }
        barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        return submit(error);
    }

    bool read_back(Image &out, std::string &error) override
    {
        Buffer b;
        const VkDeviceSize size = VkDeviceSize(extent_.width) * extent_.height * 4;
        if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b))
        {
            error = "could not create the readback buffer";
            return false;
        }
        const VkImage image = images_[current_];
        begin();
        barrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkBufferImageCopy region = copy_region(extent_.width, extent_.height);
        vk_.vkCmdCopyImageToBuffer(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buffer, 1, &region);
        barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        bool ok = submit(error);
        void *data = nullptr;
        if (ok && vk_.vkMapMemory(device_, b.memory, 0, size, 0, &data) == VK_SUCCESS)
        {
            decode(static_cast<const uint8_t *>(data), size_t(extent_.width) * 4, extent_.width, extent_.height,
                   o_.format, bgra_, out);
            vk_.vkUnmapMemory(device_, b.memory);
        }
        else if (ok)
        {
            error = "could not map the readback buffer";
            ok = false;
        }
        destroy(b);
        return ok;
    }

    bool present(std::string &error) override
    {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &current_;
        const VkResult r = vk_.vkQueuePresentKHR(queue_, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        {
            error = "vkQueuePresentKHR failed " + std::to_string(r);
            return false;
        }
        vk_.vkQueueWaitIdle(queue_);
        return true;
    }

private:
    // Loads vulkan-1.dll, creates the Vulkan instance and a surface for the window. Sets
    // `unsupported` if HDR10 was asked for and the system cannot provide it.
    bool create_instance(std::string &error, bool &unsupported)
    {
        lib_ = LoadLibraryW(L"vulkan-1.dll");
        if (lib_ == nullptr)
        {
            error = "vulkan-1.dll not found (no Vulkan driver)";
            return false;
        }
        vk_.vkGetInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void *>(GetProcAddress(lib_, "vkGetInstanceProcAddr")));
        vk_.vkCreateInstance =
            reinterpret_cast<PFN_vkCreateInstance>(vk_.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                    VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "reshade-retroarch test host";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = o_.hdr10 ? 3 : 2; // the colour space extension only for HDR10
        ci.ppEnabledExtensionNames = extensions;
        const VkResult created = vk_.vkCreateInstance != nullptr ? vk_.vkCreateInstance(&ci, nullptr, &instance_)
                                                                  : VK_ERROR_INITIALIZATION_FAILED;
        if (created != VK_SUCCESS)
        {
            // The VkResult tells e.g. a missing layer (-6) from a missing extension (-7).
            error = "vkCreateInstance failed (VkResult " + std::to_string(int(created)) + ")" +
                    (o_.hdr10 ? " (no HDR colour space support?)" : "");
            unsupported = o_.hdr10;
            return false;
        }
#define VK_LOAD(name) vk_.name = reinterpret_cast<PFN_##name>(vk_.vkGetInstanceProcAddr(instance_, #name));
        VK_LOAD(vkDestroyInstance)
        VK_LOAD(vkEnumeratePhysicalDevices)
        VK_LOAD(vkGetPhysicalDeviceQueueFamilyProperties)
        VK_LOAD(vkGetPhysicalDeviceMemoryProperties)
        VK_LOAD(vkGetPhysicalDeviceSurfaceSupportKHR)
        VK_LOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        VK_LOAD(vkGetPhysicalDeviceSurfaceFormatsKHR)
        VK_LOAD(vkGetPhysicalDeviceSurfacePresentModesKHR)
        VK_LOAD(vkCreateWin32SurfaceKHR)
        VK_LOAD(vkDestroySurfaceKHR)
        VK_LOAD(vkCreateDevice)
        VK_LOAD(vkGetDeviceProcAddr)
#undef VK_LOAD
        VkWin32SurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        si.hinstance = GetModuleHandleW(nullptr);
        si.hwnd = o_.hwnd;
        if (vk_.vkCreateWin32SurfaceKHR(instance_, &si, nullptr, &surface_) != VK_SUCCESS)
        {
            error = "vkCreateWin32SurfaceKHR failed";
            return false;
        }
        return true;
    }

    // Picks the first GPU with a queue that can both draw and present to the window, and
    // creates the device plus the command buffer and fence the host reuses.
    bool create_device(std::string &error)
    {
        uint32_t n = 0;
        vk_.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        std::vector<VkPhysicalDevice> gpus(n);
        vk_.vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
        for (VkPhysicalDevice gpu : gpus)
        {
            uint32_t qn = 0;
            vk_.vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vk_.vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qf.data());
            for (uint32_t i = 0; i < qn; ++i)
            {
                VkBool32 present = VK_FALSE;
                vk_.vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface_, &present);
                if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                {
                    gpu_ = gpu;
                    family_ = i;
                    break;
                }
            }
            if (gpu_ != VK_NULL_HANDLE)
                break;
        }
        if (gpu_ == VK_NULL_HANDLE)
        {
            error = "no Vulkan device can present to the window";
            return false;
        }
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family_;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = extensions;
        if (vk_.vkCreateDevice(gpu_, &di, nullptr, &device_) != VK_SUCCESS)
        {
            error = "vkCreateDevice failed";
            return false;
        }
#define VK_LOAD(name) vk_.name = reinterpret_cast<PFN_##name>(vk_.vkGetDeviceProcAddr(device_, #name));
        VK_LOAD(vkDestroyDevice)
        VK_LOAD(vkGetDeviceQueue)
        VK_LOAD(vkCreateSwapchainKHR)
        VK_LOAD(vkDestroySwapchainKHR)
        VK_LOAD(vkGetSwapchainImagesKHR)
        VK_LOAD(vkAcquireNextImageKHR)
        VK_LOAD(vkQueuePresentKHR)
        VK_LOAD(vkCreateCommandPool)
        VK_LOAD(vkDestroyCommandPool)
        VK_LOAD(vkAllocateCommandBuffers)
        VK_LOAD(vkBeginCommandBuffer)
        VK_LOAD(vkEndCommandBuffer)
        VK_LOAD(vkCmdPipelineBarrier)
        VK_LOAD(vkCmdCopyBufferToImage)
        VK_LOAD(vkCmdCopyImageToBuffer)
        VK_LOAD(vkCmdClearColorImage)
        VK_LOAD(vkQueueSubmit)
        VK_LOAD(vkQueueWaitIdle)
        VK_LOAD(vkDeviceWaitIdle)
        VK_LOAD(vkCreateBuffer)
        VK_LOAD(vkDestroyBuffer)
        VK_LOAD(vkGetBufferMemoryRequirements)
        VK_LOAD(vkAllocateMemory)
        VK_LOAD(vkFreeMemory)
        VK_LOAD(vkBindBufferMemory)
        VK_LOAD(vkMapMemory)
        VK_LOAD(vkUnmapMemory)
        VK_LOAD(vkCreateFence)
        VK_LOAD(vkDestroyFence)
        VK_LOAD(vkWaitForFences)
        VK_LOAD(vkResetFences)
#undef VK_LOAD
        vk_.vkGetDeviceQueue(device_, family_, 0, &queue_);
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = family_;
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vk_.vkCreateCommandPool(device_, &pi, nullptr, &pool_) != VK_SUCCESS ||
            (ai.commandPool = pool_, vk_.vkAllocateCommandBuffers(device_, &ai, &cmd_)) != VK_SUCCESS ||
            vk_.vkCreateFence(device_, &fi, nullptr, &fence_) != VK_SUCCESS)
        {
            error = "could not create the command buffer";
            return false;
        }
        return true;
    }

    // Chooses the swap chain format and colour space for the requested Format, among those
    // the window's surface offers. Sets `bgra_` when the chosen format stores blue first
    // (Windows drivers often offer only that); encode/decode handle it. Sets
    // `unsupported` when the surface offers nothing suitable.
    bool pick_format(std::string &error, bool &unsupported)
    {
        uint32_t n = 0;
        vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
        struct Want
        {
            VkFormat format;
            bool bgra;
        };
        std::vector<Want> wants;
        VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        if (o_.format == Format::rgba8)
            wants = {{VK_FORMAT_R8G8B8A8_UNORM, false}, {VK_FORMAT_B8G8R8A8_UNORM, true}};
        else if (o_.format == Format::rgba8srgb)
            wants = {{VK_FORMAT_R8G8B8A8_SRGB, false}, {VK_FORMAT_B8G8R8A8_SRGB, true}};
        else
            wants = {{VK_FORMAT_A2B10G10R10_UNORM_PACK32, false}}; // same bit layout as R10G10B10A2
        if (o_.hdr10)
            space = VK_COLOR_SPACE_HDR10_ST2084_EXT;
        for (const Want &w : wants)
            for (const VkSurfaceFormatKHR &f : formats)
                if (f.format == w.format && f.colorSpace == space)
                {
                    surface_format_ = f;
                    bgra_ = w.bgra;
                    return true;
                }
        error = "the display does not offer this swap chain format";
        unsupported = true;
        return false;
    }

    // Creates (or re-creates, replacing the old one) the swap chain at `w` x `h` and gets
    // its images. The images must allow copies in and out (TRANSFER_DST/SRC usage).
    bool create_swapchain(UINT w, UINT h, std::string &error, bool &unsupported)
    {
        VkSurfaceCapabilitiesKHR caps = {};
        vk_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if ((caps.supportedUsageFlags & usage) != usage)
        {
            error = "swap chain images cannot be copied to and from";
            unsupported = true;
            return false;
        }
        uint32_t n = 0;
        vk_.vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vk_.vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        for (VkPresentModeKHR m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR)
                mode = m;

        VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface_;
        ci.minImageCount = std::max(2u, caps.minImageCount);
        if (caps.maxImageCount != 0)
            ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
        ci.imageFormat = surface_format_.format;
        ci.imageColorSpace = surface_format_.colorSpace;
        ci.imageExtent = {w, h};
        ci.imageArrayLayers = 1;
        ci.imageUsage = usage;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = mode;
        ci.clipped = VK_FALSE; // keep every pixel, even where the window is covered
        ci.oldSwapchain = swapchain_;
        VkSwapchainKHR next = VK_NULL_HANDLE;
        if (vk_.vkCreateSwapchainKHR(device_, &ci, nullptr, &next) != VK_SUCCESS)
        {
            error = "vkCreateSwapchainKHR failed";
            return false;
        }
        if (swapchain_ != VK_NULL_HANDLE)
            vk_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = next;
        extent_ = ci.imageExtent;
        vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        images_.resize(n);
        vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
        return true;
    }

    // Creates buffer `b` of `size` bytes in CPU-visible, coherent memory, so the CPU can
    // write pictures into it and read frames out of it without explicit flushes.
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer &b)
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vk_.vkCreateBuffer(device_, &bi, nullptr, &b.buffer) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req = {};
        vk_.vkGetBufferMemoryRequirements(device_, b.buffer, &req);
        VkPhysicalDeviceMemoryProperties props = {};
        vk_.vkGetPhysicalDeviceMemoryProperties(gpu_, &props);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
                type = i;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (type == UINT32_MAX || vk_.vkAllocateMemory(device_, &ai, nullptr, &b.memory) != VK_SUCCESS ||
            vk_.vkBindBufferMemory(device_, b.buffer, b.memory, 0) != VK_SUCCESS)
        {
            destroy(b);
            return false;
        }
        return true;
    }

    // Frees buffer `b` and its memory.
    void destroy(Buffer &b)
    {
        if (b.buffer != VK_NULL_HANDLE)
            vk_.vkDestroyBuffer(device_, b.buffer, nullptr);
        if (b.memory != VK_NULL_HANDLE)
            vk_.vkFreeMemory(device_, b.memory, nullptr);
        b = {};
    }

    // Returns a buffer holding `img` in the swap chain's format, created on first use and
    // kept for the next frames. Null on failure.
    const Buffer *upload(const Image &img)
    {
        if (auto it = uploads_.find(&img); it != uploads_.end())
            return &it->second;
        Buffer b;
        const std::vector<uint8_t> bytes = encode(img, o_.format, bgra_);
        void *data = nullptr;
        if (!create_buffer(bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, b) ||
            vk_.vkMapMemory(device_, b.memory, 0, bytes.size(), 0, &data) != VK_SUCCESS)
        {
            destroy(b);
            return nullptr;
        }
        std::memcpy(data, bytes.data(), bytes.size());
        vk_.vkUnmapMemory(device_, b.memory);
        return &(uploads_[&img] = b);
    }

    // Copy description for a whole `w` x `h` image to or from a tightly packed buffer.
    static VkBufferImageCopy copy_region(uint32_t w, uint32_t h)
    {
        VkBufferImageCopy r = {};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        return r;
    }

    // Starts recording the command buffer.
    void begin()
    {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_.vkBeginCommandBuffer(cmd_, &bi);
    }

    // Records a layout transition of `image` from `from` to `to`. Vulkan requires images to
    // be in the right layout for each use (copy destination, presentation...). The barrier
    // waits for all earlier work, which is simple and always correct.
    void barrier(VkImage image, VkImageLayout from, VkImageLayout to)
    {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
    }

    // Finishes the command buffer, runs it on the GPU and waits until it is done.
    bool submit(std::string &error)
    {
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        if (vk_.vkEndCommandBuffer(cmd_) != VK_SUCCESS || vk_.vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS ||
            vk_.vkQueueWaitIdle(queue_) != VK_SUCCESS)
        {
            error = "command submission failed";
            return false;
        }
        return true;
    }

    Options o_;
    Vk vk_;
    HMODULE lib_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surface_format_ = {};
    bool bgra_ = false;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_ = {};
    std::vector<VkImage> images_;
    uint32_t current_ = 0; // index of the swap chain image acquired by the last draw
    std::map<const Image *, Buffer> uploads_;
};
}

std::unique_ptr<Backend> make_vulkan()
{
    return std::make_unique<Vulkan>();
}
