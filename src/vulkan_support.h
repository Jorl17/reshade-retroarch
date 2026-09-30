#pragma once
// What the add-on needs, besides ReShade's API, to run librashader on a game's Vulkan device.
//
// 1. An instance and physical device (a workaround). librashader's Vulkan runtime needs the
//    game's VkInstance and VkPhysicalDevice besides its VkDevice, but ReShade (6.8) gives
//    add-ons only the device and its queues. librashader uses the two only to look up the
//    GPU's memory types and properties, and to find vkGetDeviceProcAddr. So the add-on creates
//    a Vulkan instance of its own and picks, in it, the physical device that is the same GPU
//    as the game's (same LUID, the GPU's locally unique identifier, which ReShade does report).
//    Those queries then return the same results as for the game's own objects. Mixing objects
//    of two instances is outside the Vulkan specification, so this is a workaround. The
//    proper fix needs ReShade to give add-ons the game's instance and physical device.
// 2. The queue family of the game's queue, which command pools must be created for. ReShade
//    does not report it either; it is found from the queue's capabilities (see
//    vulkan_handles()).
// 3. The few device functions the add-on calls itself (VulkanFunctions).
//
// Every Vulkan call made through these goes through the Vulkan loader and so through
// ReShade's layer, like the game's own calls: ReShade handles the add-on's (and
// librashader's) command buffers and objects as if the game had made them.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES // Vulkan functions are looked up at run time, never linked
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

// The information about the game's Vulkan device that the add-on uses (see ChainDevice in
// chain.h).
struct VulkanHandles
{
    uint64_t instance = 0;                  // VkInstance of the add-on's own instance
    uint64_t physical_device = 0;           // VkPhysicalDevice in it for the game's GPU
    void *get_instance_proc_addr = nullptr; // vkGetInstanceProcAddr of vulkan-1.dll
    void *get_device_proc_addr = nullptr;   // vkGetDeviceProcAddr of vulkan-1.dll
    uint32_t queue_family = 0;              // queue family of the game's queue
};

// Fills `out` for the GPU whose LUID is `luid` (8 bytes), or, when `luid` is null, for the
// only GPU if there is exactly one. `queue_flags` are the capabilities of the game's queue
// (VkQueueFlags, as reshade::api::command_queue::get_type() returns them); the queue family
// is the GPU's only family with exactly those capabilities. The instance is created once and
// kept until the process ends. Returns false and sets `error` (a message for the user) if
// Vulkan cannot be loaded, no GPU matches, or the queue family cannot be determined.
bool vulkan_handles(const uint8_t *luid, uint32_t queue_flags, VulkanHandles &out, std::string &error);

// Device functions the add-on calls itself, looked up for one VkDevice.
struct VulkanFunctions
{
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;

    // Looks every function up for `device` with `get_device_proc_addr` (vkGetDeviceProcAddr).
    // Returns false if one is missing.
    bool load(VkDevice device, void *get_device_proc_addr);
};

// Records commands with `record` into a new command buffer on `queue_family`, submits it to
// `queue` and waits until the GPU has run it. For one-off work (setting up a filter chain),
// not for every frame. Returns false and sets `error` if a Vulkan call fails or `record`
// returns false (then `record` sets `error`).
template <typename Record>
bool vulkan_run_once(const VulkanFunctions &vk, VkDevice device, VkQueue queue, uint32_t queue_family,
                     Record record, std::string &error)
{
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vk.CreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
    {
        error = "could not create a Vulkan command pool";
        return false;
    }
    VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool ok = vk.AllocateCommandBuffers(device, &alloc, &cmd) == VK_SUCCESS &&
              vk.CreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS &&
              vk.BeginCommandBuffer(cmd, &begin) == VK_SUCCESS;
    if (!ok)
        error = "could not create a Vulkan command buffer";
    else if (!record(cmd))
        ok = false; // `record` set the error
    else
    {
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        ok = vk.EndCommandBuffer(cmd) == VK_SUCCESS && vk.QueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS &&
             vk.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
        if (!ok)
            error = "a Vulkan command buffer could not be run";
    }
    if (fence != VK_NULL_HANDLE)
        vk.DestroyFence(device, fence, nullptr);
    vk.DestroyCommandPool(device, pool, nullptr); // also frees `cmd`
    return ok;
}
