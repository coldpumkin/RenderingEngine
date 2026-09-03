#pragma once

#include "Vulkan/Core.h"

// Instance
// ============================================================================
//
// The table and the handle sit together because a call goes through one table or the
// other (measured: 26 instance-level, 30 device-level), and taking both from one place
// is what keeps them from being mixed.
//
// They are needed in different places, though. The table is read almost everywhere;
// the handle only where something is created or destroyed, because a physical-device
// query dispatches on the gpu rather than on the instance.
struct VulkanInstance {
    VolkInstanceTable table{};
    VkInstance handle = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    VulkanInstance() = default;
    ~VulkanInstance();
    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;
};

// loader check -> instance -> function table -> debug messenger.
// On failure the handle comes back VK_NULL_HANDLE, which the destructor tolerates.
bool CreateInstance(VulkanInstance* out) noexcept;
