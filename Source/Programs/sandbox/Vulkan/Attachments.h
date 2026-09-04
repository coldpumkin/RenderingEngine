#pragma once

// Attachment formats - what a pipeline is compiled to draw into
// ============================================================================
//
// **A projection of the TextureDescs, not a second source.** Every field here is one
// of theirs -- a colour format, a depth format, a sample count -- and the two they
// have that this does not are the two a pipeline never sees:
//
//   extent   the viewport is dynamic, and a swapchain's size is not known until one
//            is acquired while its format is settled long before
//   usage    what an image is for, which is the resource's business
//
// It exists because Vulkan asks for exactly this at pipeline creation, before any
// image does. The caller derives it from the descs it already wrote; nothing here
// invents a value, so the images and the pipeline cannot come to disagree.

#include "Vulkan/Texture.h"

// The contract between the images we create and the pipeline that draws into them.
//
// Dynamic rendering bakes all three of these into the pipeline, so both sides
// have to read the same values. One struct rather than three arguments means
// passing it whole is enough to keep them in step.
//
// Here rather than in VulkanDevice because the candidate list and its priority
// are our render target's policy. The GPU only answers "is this supported".
struct AttachmentFormats {
    // UNDEFINED means this pass draws no colour, and it is not a value a caller
    // chooses freely: the fragment stage decides by declaring an output or not, and
    // CreateGraphicsPipeline refuses the pair that disagrees. A depth-only pass leaves
    // it at the default rather than spelling out an absence the .spv already states.
    VkFormat color = VK_FORMAT_UNDEFINED;

    // UNDEFINED means no depth. No shader says so -- depth is fixed-function -- so
    // unlike the field above, this one really is the pass's to choose.
    VkFormat depth = VK_FORMAT_UNDEFINED;
    // Highest count both color and depth support, capped by kDesiredSampleCount.
    // 1 would mean no MSAA, which the resolve path does not handle (Config.h).
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// Output: the pipeline's view of the images a pass draws into
//
// **The one projection.** Either side may be absent -- a depth-only pass has no
// colour, a swapchain image has no depth -- and UNDEFINED is what says so.
//
// It exists so that "what do I draw into" is answered once, from the descs, wherever
// it is asked. Three functions used to do this, one per producer, and before them
// main wrote the fields out by hand.
//
// Contract: when both are given their sample counts must match. One
//           rasterizationSamples covers every attachment in a pass, so there is no
//           pipeline that could honour two.
AttachmentFormats AttachmentFormatsOf(const TextureDesc* color,
                                      const TextureDesc* depth) noexcept;

// The comparison the pass creations make: the descs they were handed, projected, and
// what their pipeline actually baked. Nobody else can see both ends.
inline bool SameAttachmentFormats(const AttachmentFormats& a,
                                  const AttachmentFormats& b) noexcept {
    return a.color == b.color && a.depth == b.depth && a.samples == b.samples;
}


// What only the device can answer about our render targets
//
// The two values a caller cannot decide: which depth format exists here, and how many
// samples colour and depth both support. Everything else about a target -- its size,
// its colour, what it is used for -- is the caller's, and goes into a TextureDesc.
struct TargetCapabilities {
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// Input:  depthUsage is everything the depth targets do between them. A format that
//         can do only part of it is no answer, because they share the one format.
// Output: false means this GPU cannot run our render targets - no depth format does
//         all of that, or there is no multisampling, which the resolve path requires.
//
// **A search, and only a search.** Whether a format can do what an image asks is not
// here any more: CreateImage2D asks that of every image, from its own usage. What is
// left is the two questions with more than one right answer, and picking among those
// is a policy rather than a check.
//
// Takes inst because these queries are instance level. No logical device needed.
bool QueryTargetCapabilities(const VulkanInstance& inst, VkPhysicalDevice gpu,
                             VkImageUsageFlags depthUsage, uint32_t desiredSamples,
                             TargetCapabilities* out) noexcept;
