#include "Vulkan/Attachments.h"

#include "Vulkan/Barrier.h"   // the transitions an attachment implies
#include "Vulkan/Image.h"     // RequiredFormatFeatures

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

AttachmentFormats AttachmentFormatsOf(const TextureDesc* const targets[],
                                      uint32_t count) noexcept {
    AttachmentFormats formats{};

    // The first desc given decides samples, and every other one is compared to it.
    // A pass with no targets at all never reaches a pipeline, so the default stands.
    const TextureDesc* first = nullptr;

    for (uint32_t i = 0; i < count && targets[i] != nullptr; ++i) {
        const TextureDesc& target = *targets[i];

        // The usage bits decide which slot this is. A desc carrying both is not a
        // thing Vulkan has, and one carrying neither is not a render target.
        const bool isColour = (target.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const bool isDepth =
            (target.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

        if (isColour == isDepth) {
            LOG("[vk] target %u has usage 0x%x, which is neither a colour nor a depth"
                " attachment\n", i, target.usage);
            continue;
        }
        if (isColour) {
            if (formats.colorCount >= kMaxColorTargets) {
                LOG("[vk] more than %u colour targets\n", kMaxColorTargets);
                continue;
            }
            formats.color[formats.colorCount] = target.format;
            formats.colorCount += 1;
        } else {
            if (formats.depth != VK_FORMAT_UNDEFINED) {
                LOG("[vk] target %u is a second depth attachment\n", i);
                continue;
            }
            formats.depth = target.format;
        }

        if (first == nullptr) { first = &target; }
        else if (target.samples != first->samples) {
            LOG("[vk] target %u is %d-sample where the first is %d-sample\n",
                i, static_cast<int>(target.samples), static_cast<int>(first->samples));
        }
    }

    if (first != nullptr) { formats.samples = first->samples; }
    return formats;
}

bool QueryTargetCapabilities(const VulkanInstance& inst, VkPhysicalDevice gpu,
                             VkImageUsageFlags depthUsage, uint32_t desiredSamples,
                             TargetCapabilities* out) noexcept {
    TargetCapabilities& formats = *out;

    // Most precise first, and stencil-free ahead of stencil since we never use
    // stencil: carrying it costs memory and puts another aspectMask on every
    // barrier and view. optimalTiling because render targets are never linear.
    //
    // What each candidate has to satisfy comes from the usage handed in -- so a GPU
    // where D32_SFLOAT can be drawn into but not sampled moves on to the next one
    // instead of being found out at the shadow map.
    const VkFormatFeatureFlags neededDepth = RequiredFormatFeatures(depthUsage);
    for (const VkFormat candidate : {VK_FORMAT_D32_SFLOAT,
                                     VK_FORMAT_X8_D24_UNORM_PACK32,
                                     VK_FORMAT_D32_SFLOAT_S8_UINT,
                                     VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties props{};
        inst.table.vkGetPhysicalDeviceFormatProperties(gpu, candidate, &props);
        if ((props.optimalTilingFeatures & neededDepth) == neededDepth) {
            formats.depthFormat = candidate;
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
        if (static_cast<uint32_t>(candidate) > desiredSamples) { continue; }
        if ((supported & candidate) != 0) {
            formats.samples = candidate;
            break;
        }
    }

    LOG("MSAA: requested %ux, supported mask 0x%x, using %ux\n",
        desiredSamples, supported, static_cast<uint32_t>(formats.samples));

    // Both failures live here, not at the call site: the caller would have to know
    // that UNDEFINED and 1_BIT are the sentinels.
    if (formats.depthFormat == VK_FORMAT_UNDEFINED) {
        LOG("[vk] no usable depth format\n");
        return false;
    }
    if (formats.samples == VK_SAMPLE_COUNT_1_BIT) {
        LOG("[vk] no multisampling: the resolve path has no 1x fallback\n");
        return false;
    }
    return true;
}

bool BeginPass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
               const RenderPassDesc& desc,
               const Texture* const views[], const Texture* const resolves[],
               uint32_t count, VkRect2D area,
               VkPipelineStageFlags2 waitedStage) noexcept {
    if (count != desc.useCount) {
        LOG("[vk] a pass declaring %u attachments was handed %u views\n",
            desc.useCount, count);
        return false;
    }
    if (count > kMaxColorTargets + 1) {
        LOG("[vk] a pass of %u attachments, and we hold %u\n", count, kMaxColorTargets + 1);
        return false;
    }

    VkRenderingAttachmentInfo colour[kMaxColorTargets]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t colourCount = 0;
    bool haveDepth = false;

    for (uint32_t i = 0; i < count; ++i) {
        if (views[i] == nullptr) {
            LOG("[vk] attachment %u has no view\n", i);
            return false;
        }
        const AttachmentUse& use = desc.uses[i];

        // The role is the image's, out of the usage it was made with -- the same rule
        // AttachmentFormatsOf reads, so a pass and its pipeline cannot disagree about
        // which slot is which. The layout follows from the role and is not a choice.
        const bool isColour =
            (views[i]->desc.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const bool isDepth =
            (views[i]->desc.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        if (isColour == isDepth) {
            LOG("[vk] attachment %u is neither a colour nor a depth target\n", i);
            return false;
        }

        VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        info.imageView = views[i]->view.handle;
        info.imageLayout = isColour ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                    : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        info.loadOp = use.load;
        info.storeOp = use.store;
        info.clearValue = use.clear;

        if (use.resolve != VK_RESOLVE_MODE_NONE) {
            const Texture* into = resolves != nullptr ? resolves[i] : nullptr;
            if (into == nullptr) {
                LOG("[vk] attachment %u resolves and was given nowhere to resolve to\n", i);
                return false;
            }
            info.resolveMode = use.resolve;
            info.resolveImageView = into->view.handle;
            info.resolveImageLayout = info.imageLayout;
        }

        // Where it has to be before the first draw. Skipped for LOAD, which reads what
        // came before and so has a writer to issue it instead.
        if (use.load != VK_ATTACHMENT_LOAD_OP_LOAD) {
            RecordAttachmentTransition(vk, cmd, views[i]->image.handle,
                                       isColour ? VK_IMAGE_ASPECT_COLOR_BIT
                                                : VK_IMAGE_ASPECT_DEPTH_BIT,
                                       info, waitedStage);
        }

        if (isColour) {
            if (colourCount >= kMaxColorTargets) {
                LOG("[vk] more than %u colour attachments\n", kMaxColorTargets);
                return false;
            }
            colour[colourCount] = info;
            colourCount += 1;
        } else {
            if (haveDepth) {
                LOG("[vk] attachment %u is a second depth attachment\n", i);
                return false;
            }
            depth = info;
            haveDepth = true;
        }
    }

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.flags = desc.flags;
    rendering.renderArea = area;
    rendering.layerCount = desc.layerCount;
    rendering.viewMask = desc.viewMask;
    rendering.colorAttachmentCount = colourCount;
    rendering.pColorAttachments = colourCount != 0 ? colour : nullptr;
    rendering.pDepthAttachment = haveDepth ? &depth : nullptr;

    vk.vkCmdBeginRendering(cmd, &rendering);
    return true;
}
