#pragma once

// Render targets - where we draw. Nothing here touches the swapchain.
// ============================================================================
//
// The scene pass draws here; the present pass copies the result out. So the
// direction is one way, and rendering works with no window at all.
//
// Three images, and what happens to each is most of this file:
//
//   color     drawn into, MSAA. Discarded once it has been resolved
//   resolve   filled by vkCmdEndRendering, 1-sample. Read by the next pass, so it
//             carries the set that reads it -- the only one of the three that leaves
//   depth     tested and written, MSAA. Never leaves the frame
//
// A FrameSlot owns one set of these because the count comes from kFramesInFlight -
// how many frames are drawn at once, not how many swapchain images exist.
//
// Separate from Frame.h because the two change for different reasons: semaphores
// and fences there, the shape of what we draw into here.

#include "Vulkan/Texture.h"

struct RenderTargets {
    // color and resolve differ in exactly one thing: sample count. A multisample
    // image cannot be read through sampler2D, so the present pass needs its own.
    Image color;
    Texture resolve;
    Image depth;
    VkExtent2D extent{};
};

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

// Effect: creates the three images and the set the present pass reads.
bool CreateRenderTargets(const VulkanDevice& dev,
                         VkExtent2D extent, RenderTargetFormats formats,
                         RenderTargets* out) noexcept;
