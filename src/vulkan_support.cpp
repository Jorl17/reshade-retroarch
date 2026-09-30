// Implementation of vulkan_support.h: the add-on's own Vulkan instance, finding the game's
// GPU and queue family in it, and loading device functions.

#include "vulkan_support.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <type_traits>
#include <vector>

bool vulkan_handles(const uint8_t *luid, uint32_t queue_flags, VulkanHandles &out, std::string &error)
{
    static std::mutex mutex;
    static VkInstance instance = VK_NULL_HANDLE;
    static PFN_vkGetInstanceProcAddr gipa = nullptr;
    static PFN_vkGetDeviceProcAddr gdpa = nullptr;
    std::lock_guard<std::mutex> lock(mutex);

    // The game has loaded the Vulkan loader already; use the same one.
    if (gipa == nullptr)
    {
        HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
        if (loader == nullptr)
            loader = LoadLibraryW(L"vulkan-1.dll");
        if (loader != nullptr)
        {
            gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                reinterpret_cast<void *>(GetProcAddress(loader, "vkGetInstanceProcAddr")));
            gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                reinterpret_cast<void *>(GetProcAddress(loader, "vkGetDeviceProcAddr")));
        }
        if (gipa == nullptr || gdpa == nullptr)
        {
            gipa = nullptr;
            error = "the Vulkan loader (vulkan-1.dll) could not be found";
            return false;
        }
    }
    if (instance == VK_NULL_HANDLE)
    {
        const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "RetroArch Shaders (ReShade add-on)";
        app.apiVersion = VK_API_VERSION_1_1; // for vkGetPhysicalDeviceProperties2
        VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        if (create == nullptr || create(&ci, nullptr, &instance) != VK_SUCCESS)
        {
            instance = VK_NULL_HANDLE;
            error = "could not create a Vulkan instance";
            return false;
        }
    }

    const auto enumerate =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    const auto properties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
    const auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    uint32_t count = 0;
    enumerate(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    enumerate(instance, &count, gpus.data());

    // The game's GPU: the one with the same LUID, or the only one when ReShade gave no LUID.
    VkPhysicalDevice found = VK_NULL_HANDLE;
    if (luid == nullptr)
    {
        if (count == 1)
            found = gpus[0];
    }
    else
    {
        for (VkPhysicalDevice gpu : gpus)
        {
            VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
            properties2(gpu, &props);
            if (id.deviceLUIDValid && std::memcmp(id.deviceLUID, luid, VK_LUID_SIZE) == 0)
                found = gpu;
        }
    }
    if (found == VK_NULL_HANDLE)
    {
        error = "could not find the game's GPU among the Vulkan devices";
        return false;
    }

    // The game's queue family: the only one whose capabilities are exactly the queue's. On
    // the GPUs tested so far every family has a different set, so this identifies it; if two
    // families had the same set, the queue could be in either, so return an error instead
    // of picking one.
    uint32_t nfam = 0;
    families(found, &nfam, nullptr);
    std::vector<VkQueueFamilyProperties> fam(nfam);
    families(found, &nfam, fam.data());
    uint32_t matches = 0;
    for (uint32_t i = 0; i < nfam; ++i)
        if (fam[i].queueFlags == queue_flags)
        {
            if (matches++ == 0)
                out.queue_family = i;
        }
    if (matches != 1)
    {
        error = matches == 0 ? "could not find the queue family of the game's Vulkan queue"
                             : "the game's Vulkan queue could be in several queue families (they have the same "
                               "capabilities)";
        return false;
    }

    out.instance = reinterpret_cast<uint64_t>(instance);
    out.physical_device = reinterpret_cast<uint64_t>(found);
    out.get_instance_proc_addr = reinterpret_cast<void *>(gipa);
    out.get_device_proc_addr = reinterpret_cast<void *>(gdpa);
    return true;
}

bool VulkanFunctions::load(VkDevice device, void *get_device_proc_addr)
{
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_device_proc_addr);
    if (gdpa == nullptr)
        return false;
    // Looks `name` up and stores it in `fn` (a function pointer of the matching type).
    const auto get = [&](auto &fn, const char *name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(gdpa(device, name));
        return fn != nullptr;
    };
    return get(CreateCommandPool, "vkCreateCommandPool") && get(DestroyCommandPool, "vkDestroyCommandPool") &&
           get(AllocateCommandBuffers, "vkAllocateCommandBuffers") &&
           get(ResetCommandBuffer, "vkResetCommandBuffer") && get(BeginCommandBuffer, "vkBeginCommandBuffer") &&
           get(EndCommandBuffer, "vkEndCommandBuffer") && get(CmdPipelineBarrier, "vkCmdPipelineBarrier") &&
           get(QueueSubmit, "vkQueueSubmit") && get(CreateFence, "vkCreateFence") &&
           get(DestroyFence, "vkDestroyFence") && get(WaitForFences, "vkWaitForFences") &&
           get(ResetFences, "vkResetFences");
}
