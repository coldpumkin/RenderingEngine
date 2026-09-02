#include "Vulkan/Attachments.h"

#include "Config.h"

#include <initializer_list>   // the candidate loops below

// Independent of the swapchain format - the present stage sits between the two.
// HDR is the change that would make this R16G16B16A16_SFLOAT.
//
// Kept out of the header so nothing can read it directly and bypass
// AttachmentFormats.
static constexpr VkFormat kRenderColorFormat = VK_FORMAT_R8G8B8A8_SRGB;

bool ChooseAttachmentFormats(const VulkanInstance& inst, VkPhysicalDevice gpu,
                               AttachmentFormats* out) noexcept {
    AttachmentFormats& formats = *out;
    formats.color = kRenderColorFormat;

    // Most precise first, and stencil-free ahead of stencil since we never use
    // stencil: carrying it costs memory and puts another aspectMask on every
    // barrier and view. optimalTiling because render targets are never linear.
    for (const VkFormat candidate : {VK_FORMAT_D32_SFLOAT,
                                     VK_FORMAT_X8_D24_UNORM_PACK32,
                                     VK_FORMAT_D32_SFLOAT_S8_UINT,
                                     VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties props{};
        inst.table.vkGetPhysicalDeviceFormatProperties(gpu, candidate, &props);
        if ((props.optimalTilingFeatures
             & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            formats.depth = candidate;
            break;
        }
    }

    // Color and depth must land on the same count: one rasterizationSamples
    // covers every attachment in the pass. Hence the intersection.
    //
    // These are device limits, not format properties, so unlike the loop above
    // there is nothing to ask per format.
    VkPhysicalDeviceProperties props{};
    inst.table.vkGetPhysicalDeviceProperties(gpu, &props);
    const VkSampleCountFlags supported = props.limits.framebufferColorSampleCounts
                                         & props.limits.framebufferDepthSampleCounts;

    // Highest first. A VkSampleCountFlagBits is its own sample count (4_BIT == 0x4),
    // so it compares against the request directly and needs no lookup table.
    for (const VkSampleCountFlagBits candidate : {VK_SAMPLE_COUNT_8_BIT,
                                                  VK_SAMPLE_COUNT_4_BIT,
                                                  VK_SAMPLE_COUNT_2_BIT}) {
        if (static_cast<uint32_t>(candidate) > kDesiredSampleCount) { continue; }
        if ((supported & candidate) != 0) {
            formats.samples = candidate;
            break;
        }
    }

    LOG("MSAA: requested %ux, supported mask 0x%x, using %ux\n",
        kDesiredSampleCount, supported, static_cast<uint32_t>(formats.samples));

    // Both failures live here, not at the call site: the caller would have to know
    // that UNDEFINED and 1_BIT are the sentinels.
    if (formats.depth == VK_FORMAT_UNDEFINED) {
        LOG("[vk] no usable depth format\n");
        return false;
    }
    if (formats.samples == VK_SAMPLE_COUNT_1_BIT) {
        LOG("[vk] no multisampling: the resolve path has no 1x fallback\n");
        return false;
    }
    return true;
}
