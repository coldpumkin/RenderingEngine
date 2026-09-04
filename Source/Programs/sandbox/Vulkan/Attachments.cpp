#include "Vulkan/Attachments.h"

#include "Config.h"

#include <initializer_list>   // the candidate loops below

// Where sRGB actually matters, counted rather than assumed
// ----------------------------------------------------------------------------
//
// The fragment stage writes linear light. Walking the chain for both choices of the
// colour format, against a swapchain that is sRGB:
//
//   SRGB   store E(L)  ->  post samples, hardware decodes to L  ->  writes L,
//                          swapchain encodes  ->  screen gets E(L)   correct
//   UNORM  store L     ->  post samples L                       ->  writes L,
//                          swapchain encodes  ->  screen gets E(L)   correct
//
// **Only the swapchain's format decides whether the picture is right**, and
// SelectSurfaceFormat refuses a surface that has no sRGB for exactly that reason.
//
// So the colour a pass draws into is not this file's to pick. It used to be, as a
// private constant here, which put a value under a subject that has no claim on it.
// The caller fills it now and this function answers only what the GPU can: whether
// that format works, which depth format exists, and how many samples.

bool ChooseAttachmentFormats(const VulkanInstance& inst, VkPhysicalDevice gpu,
                               AttachmentFormats* out) noexcept {
    AttachmentFormats& formats = *out;

    // Asked about, not assumed. Depth has been asked about since this function existed
    // and colour never was -- it was assigned from a constant here and the GPU was
    // left out of it, which was an odd pair of habits for two fields of one struct.
    //
    // Both bits, because this image is two things: drawn into by the pass that owns it
    // and sampled by whatever reads it next. A format supporting one and not the other
    // would fail at the second image rather than here.
    {
        VkFormatProperties props{};
        inst.table.vkGetPhysicalDeviceFormatProperties(gpu, formats.color, &props);
        constexpr VkFormatFeatureFlags needed =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((props.optimalTilingFeatures & needed) != needed) {
            LOG("[vk] colour format %d cannot be both drawn into and sampled\n",
                static_cast<int>(formats.color));
            return false;
        }
    }

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
