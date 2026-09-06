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

// Output: what the role's format lands in, or nullptr if the role cannot be honoured
//
// The two facts a role is refused by. A format settles which of the three an image can
// ever be; a usage bit settles whether it was made able to be drawn into at all.
static VkFormat* SlotForRole(const Attachment& a, AttachmentFormats* out,
                             uint32_t index) noexcept {
    const VkImageAspectFlags aspect = AspectOfFormat(a.resource->format);
    const bool formatIsDepth = aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    const bool wantsDepth = a.role != AttachmentRole::Color;
    if (formatIsDepth != wantsDepth) {
        LOG("[vk] attachment %u is declared %s and its format is a %s one\n", index,
            wantsDepth ? "depth/stencil" : "colour", formatIsDepth ? "depth" : "colour");
        return nullptr;
    }

    const VkImageUsageFlags needed = wantsDepth
                                   ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                   : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((a.resource->usage & needed) == 0) {
        LOG("[vk] attachment %u is declared %s and was not created to be drawn into"
            " as one (usage 0x%x)\n", index,
            wantsDepth ? "depth/stencil" : "colour", a.resource->usage);
        return nullptr;
    }

    switch (a.role) {
    case AttachmentRole::Depth:
        if (out->depth != VK_FORMAT_UNDEFINED) {
            LOG("[vk] attachment %u is a second depth attachment\n", index);
            return nullptr;
        }
        return &out->depth;
    case AttachmentRole::Stencil:
        if (out->stencil != VK_FORMAT_UNDEFINED) {
            LOG("[vk] attachment %u is a second stencil attachment\n", index);
            return nullptr;
        }
        return &out->stencil;
    case AttachmentRole::Color:
    default:
        if (out->colorCount >= kMaxColorTargets) {
            LOG("[vk] more than %u colour attachments\n", kMaxColorTargets);
            return nullptr;
        }
        return &out->color[out->colorCount++];
    }
}

AttachmentFormats AttachmentFormatsOf(const Attachment attachments[],
                                      uint32_t count) noexcept {
    AttachmentFormats formats{};

    // The first entry decides samples, and every other one is compared to it.
    const TextureDesc* first = nullptr;

    for (uint32_t i = 0; i < count && attachments[i].resource != nullptr; ++i) {
        const Attachment& a = attachments[i];
        VkFormat* slot = SlotForRole(a, &formats, i);
        if (slot == nullptr) { continue; }
        *slot = a.resource->format;

        if (first == nullptr) { first = a.resource; }
        else if (a.resource->samples != first->samples) {
            LOG("[vk] attachment %u is %d-sample where the first is %d-sample\n", i,
                static_cast<int>(a.resource->samples),
                static_cast<int>(first->samples));
        }
    }

    if (first != nullptr) { formats.samples = first->samples; }
    return formats;
}

AttachmentFormats AttachmentFormatsFor(const TextureDesc* const colour[],
                                       uint32_t colourCount,
                                       const TextureDesc* depth) noexcept {
    // Built into the one shape the projection reads, so there is one place that turns
    // descs and roles into a contract rather than two that could come to differ.
    //
    // No stencil argument: nothing here writes stencil, and a parameter for it would be
    // a role no caller can honour yet. The field it fills stays UNDEFINED, which is
    // what a pipeline that begins no stencil attachment must say.
    Attachment attachments[kMaxAttachments]{};

    uint32_t count = 0;
    for (uint32_t i = 0; i < colourCount && i < kMaxColorTargets; ++i) {
        attachments[count].resource = colour[i];
        attachments[count].role = AttachmentRole::Color;
        ++count;
    }
    if (depth != nullptr) {
        attachments[count].resource = depth;
        attachments[count].role = AttachmentRole::Depth;
        ++count;
    }
    return AttachmentFormatsOf(attachments, count);
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
               VkRect2D area, VkPipelineStageFlags2 waitedStage) noexcept {
    uint32_t count = 0;
    while (count < kMaxAttachments && desc.attachments[count].resource != nullptr) {
        count += 1;
    }
    if (count == 0) {
        LOG("[vk] a pass with no attachments\n");
        return false;
    }

    VkRenderingAttachmentInfo colour[kMaxColorTargets]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t colourCount = 0;
    bool haveDepth = false;

    for (uint32_t i = 0; i < count; ++i) {
        const Attachment& use = desc.attachments[i];

        // Nothing here begins a stencil attachment, and the layout and the aspect a
        // second entry over one image needs are not written. Refused rather than
        // guessed: what is missing is a STENCIL_ATTACHMENT_OPTIMAL layout here and a
        // per-aspect transition in Barrier.cpp.
        if (use.role == AttachmentRole::Stencil) {
            LOG("[vk] attachment %u is declared stencil, which BeginPass does not"
                " begin yet\n", i);
            return false;
        }

        if (views[i] == nullptr) {
            LOG("[vk] attachment %u has no view\n", i);
            return false;
        }

        // The image against the desc it stands in for. Handing these over in another
        // order is what this catches -- and it catches it wherever the two descs differ,
        // which is not everywhere: two targets of the same format and sample count are
        // still tellable apart only by where they sit.
        const TextureDesc& want = *use.resource;
        const TextureDesc& got = views[i]->desc;
        if (got.format != want.format || got.samples != want.samples
                || got.usage != want.usage) {
            LOG("[vk] attachment %u was handed an image the pass did not declare "
                "(format %d/%d, samples %d/%d)\n", i,
                static_cast<int>(got.format), static_cast<int>(want.format),
                static_cast<int>(got.samples), static_cast<int>(want.samples));
            return false;
        }

        // The role the pass declared, not one read back out of the image. The layout
        // follows from it and is not a choice.
        const bool isColour = use.role == AttachmentRole::Color;

        VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        info.imageView = views[i]->view.handle;
        info.imageLayout = isColour ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                    : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        info.loadOp = use.load;
        info.storeOp = use.store;
        info.clearValue = use.clear;

        // The declared destination and this frame's image for it, in that order. Both
        // are needed and neither implies the other: one desc is a different image every
        // frame in flight, and two descs can describe the same thing.
        if (use.resolve.target != nullptr) {
            const Texture* into = resolves != nullptr ? resolves[i] : nullptr;
            if (into == nullptr) {
                LOG("[vk] attachment %u resolves and was given nowhere to resolve to\n", i);
                return false;
            }
            info.resolveMode = use.resolve.mode;
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
