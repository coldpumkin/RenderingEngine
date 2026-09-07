#pragma once

// What every Vulkan file here shares: the log macro, what we require of a device, and
// the RAII convention every resource type follows.
//
// Every Vulkan call belongs to one of two levels:
//   instance level   dispatched on VkInstance or VkPhysicalDevice
//   device level     dispatched on VkDevice, VkQueue or VkCommandBuffer
//
// That line is the API's, and the files follow it. Each level carries its own function
// table (VolkInstanceTable / VolkDeviceTable), and the handle comes from the same
// object as the table so the two cannot be mixed.
//
// Four functions stay global, and only because they are called before an instance
// exists: volkInitialize, vkEnumerateInstanceVersion,
// vkEnumerateInstanceLayerProperties, vkCreateInstance.
//
// The RAII convention:
//   1. default construction leaves it empty, and empty is a legal state
//   2. a Create* function fills an out parameter. Returning by value would mean a
//      move constructor per type
//   3. a destructor takes no arguments, so what it needs to destroy with (dev, inst)
//      is held as a non-owning pointer
//   4. no copying -- a handle would be destroyed twice

#include <volk.h>

#include <cstdio>

#define LOG(...)  std::fprintf(stderr, __VA_ARGS__)

// What we require
// ============================================================================

// 1.3 because dynamic rendering is core there.
constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;

// The device extension needed to put anything on screen.
// (VK_KHR_surface is an instance extension -- a different level.)
constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// Supporting the version does not mean the features are on, so they are asked for
// separately.
//
// Contract: the check and the enable must read the same values. Out of step, the
//           device is created and the draw is what dies.
inline VkPhysicalDeviceVulkan13Features RequiredFeatures13() noexcept {
    VkPhysicalDeviceVulkan13Features features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.dynamicRendering = VK_TRUE;   // draw with no VkRenderPass or VkFramebuffer
    features.synchronization2 = VK_TRUE;   // the revised barrier and submit API
    return features;
}

// The core 1.0 features we require. One, and it is here because a pipeline uses it:
// POLYGON_MODE_LINE needs fillModeNonSolid, and the wireframe variant of the scene
// pipeline is built with it.
//
// Contract: the candidate check in Device.cpp reads this same function. Out of step,
//           the device is created and the draw is what dies.
inline VkPhysicalDeviceFeatures RequiredFeatures10() noexcept {
    VkPhysicalDeviceFeatures features{};
    features.fillModeNonSolid = VK_TRUE;   // POLYGON_MODE_LINE

    // Anisotropic filtering. A mip level is chosen from the larger of the two screen
    // derivatives, which over-blurs a surface seen at a grazing angle -- the footprint
    // is long in one direction and short in the other, and one level cannot be right
    // for both. Anisotropy takes several samples along the long axis instead.
    features.samplerAnisotropy = VK_TRUE;
    return features;
}
