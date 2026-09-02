#pragma once

// Render target formats - what the images and the pipeline must agree on
// ============================================================================
//
// The images themselves live in a FrameSlot. Only the formats are here, because a
// pipeline needs the same values and never sees the images.

#include "Vulkan/Device.h"

// The contract between the images we create and the pipeline that draws into them.
//
// Dynamic rendering bakes all three of these into the pipeline, so both sides
// have to read the same values. One struct rather than three arguments means
// passing it whole is enough to keep them in step.
//
// Here rather than in VulkanDevice because the candidate list and its priority
// are our render target's policy. The GPU only answers "is this supported".
struct RenderTargetFormats {
    VkFormat color = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    // Highest count both color and depth support, capped by kDesiredSampleCount.
    // 1 would mean no MSAA, which the resolve path does not handle (Config.h).
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// Input:  inst, gpu
// Output: false means this GPU cannot run our render targets - no depth format, or
//         no multisampling, which the resolve path requires.
//
// Takes inst because these queries are instance level. No logical device needed.
bool ChooseRenderTargetFormats(const VulkanInstance& inst, VkPhysicalDevice gpu,
                               RenderTargetFormats* out) noexcept;
