#pragma once

// Attachment formats - the spec a pass draws into
// ============================================================================
//
// Three values decide three Textures (color, its resolve, depth) and go unchanged
// into the pipeline, so no type has to bundle the results. extent is not here: the
// pipeline never sees it, its viewport being dynamic.

#include "Vulkan/Instance.h"

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

// Input:  inst, gpu, and out->color already filled in by the caller
// Output: false means this GPU cannot run that render target - the colour format is
//         not usable as both an attachment and a sampled image, or there is no depth
//         format, or no multisampling, which the resolve path requires.
//
// **Only what the GPU can answer.** Which colour a target is made of is the caller's,
// filled in before the call the way the shadow and swapchain formats are filled in at
// their own declarations; this fills the two fields that need a device to answer and
// checks the one it was given.
//
// Takes inst because these queries are instance level. No logical device needed.
//
// Contract: out->color is set. UNDEFINED would mean a pass that draws no colour, and
//           such a pass has no use for the rest of this either.
bool ChooseAttachmentFormats(const VulkanInstance& inst, VkPhysicalDevice gpu,
                               AttachmentFormats* out) noexcept;
