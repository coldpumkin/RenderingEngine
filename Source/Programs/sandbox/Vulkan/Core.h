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

// No core 1.0 feature is required. fillModeNonSolid was, for POLYGON_MODE_LINE, and
// no pipeline uses that any more -- a GPU was being turned away over something nothing
// asked for.
//
// Adding one back means editing here **and** the candidate check in Device.cpp: the
// check and the enable have to read the same values, and out of step the device is
// created and the draw is what dies.
