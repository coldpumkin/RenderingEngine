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
    const bool formatIsDepth = IsDepthFormat(a.resource->format);
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

        // A disagreement is not reported here. ValidatePassDesc refuses it, and this
        // runs once per frame in flight on some paths, so one fault would say three
        // things. The projection answers with the first, which is what a pipeline is
        // compiled against.
        if (first == nullptr) { first = a.resource; }
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

    // Most precise first, and every candidate is depth-only. optimalTiling because
    // render targets are never linear.
    //
    // **A combined format is not on this list, and that is a refusal rather than a
    // preference.** The aspect an image needs is not the format's -- it is the format's
    // and the use's together, and on a combined format the two uses this repo already
    // has want opposite things. A barrier over one needs both aspects named
    // (VUID-VkImageMemoryBarrier2-image-03320, since separateDepthStencilLayouts is not
    // asked for), while a view sampled from one must name exactly one
    // (VUID-VkDescriptorImageInfo-imageView-01976). The shadow map is both, so one
    // image would need two views and a layout of DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    // rather than DEPTH_ATTACHMENT_OPTIMAL, which naming both aspects rules out (08702).
    //
    // Measured before deciding: forcing the list onto D32_SFLOAT_S8_UINT draws ten
    // errors from vkCmdPipelineBarrier2 and nothing else works differently. Nothing
    // here uses stencil, so the machinery buys a format we do not want -- and a search
    // that can land on it is worse than one that cannot.
    //
    // What each candidate has to satisfy comes from the usage handed in -- so a GPU
    // where D32_SFLOAT can be drawn into but not sampled moves on to the next one
    // instead of being found out at the shadow map. D16_UNORM last: least precise, and
    // there so the refusal below stays as rare as a depth-only search can make it.
    const VkFormatFeatureFlags neededDepth = RequiredFormatFeatures(depthUsage);
    for (const VkFormat candidate : {VK_FORMAT_D32_SFLOAT,
                                     VK_FORMAT_X8_D24_UNORM_PACK32,
                                     VK_FORMAT_D16_UNORM}) {
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
        LOG("[vk] no depth-only format on this GPU does all of usage 0x%x. A combined"
            " depth/stencil one would need per-aspect barriers and a second view for"
            " every sampled depth image; see the candidate list above\n", depthUsage);
        return false;
    }
    if (formats.samples == VK_SAMPLE_COUNT_1_BIT) {
        LOG("[vk] no multisampling: the resolve path has no 1x fallback\n");
        return false;
    }
    return true;
}

// Output: whether this format carries a stencil aspect
//
// FormatAspects reports both on a combined format; this asks only whether stencil is
// among them, which is what a stencil role has to be refused by.
static bool HasStencilAspect(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
    }
}

bool ValidatePassDesc(const RenderPassDesc& desc) noexcept {
    uint32_t count = 0;
    while (count < kMaxAttachments && desc.attachments[count].resource != nullptr) {
        count += 1;
    }
    if (count == 0) {
        LOG("[vk] a pass with no attachments\n");
        return false;
    }

    const Attachment* depth = nullptr;
    const Attachment* stencil = nullptr;
    bool ok = true;

    for (uint32_t i = 0; i < count; ++i) {
        const Attachment& a = desc.attachments[i];

        // One rasterizationSamples covers a whole pass, so two attachments cannot
        // disagree -- VUID-VkRenderingInfo-multisampledRenderToSingleSampled-06857.
        // The projection used to log this and let the first one win, which meant a
        // pipeline compiled for a sample count one of its targets did not have.
        if (a.resource->samples != desc.attachments[0].resource->samples) {
            LOG("[vk] attachment %u is %d-sample where attachment 0 is %d-sample\n", i,
                static_cast<int>(a.resource->samples),
                static_cast<int>(desc.attachments[0].resource->samples));
            ok = false;
        }

        if (a.role == AttachmentRole::Depth) { depth = &a; }
        if (a.role == AttachmentRole::Stencil) {
            stencil = &a;
            // VUID-VkRenderingInfo-pStencilAttachment-06548
            if (!HasStencilAspect(a.resource->format)) {
                LOG("[vk] attachment %u is declared stencil and its format %d has no"
                    " stencil aspect\n", i, static_cast<int>(a.resource->format));
                ok = false;
            }
        }

        // The two halves of a resolve arrive or neither does -- a mode with nowhere to
        // go is VUID-VkRenderingAttachmentInfo-imageView-06862, and a destination with
        // mode NONE is a declaration that does nothing.
        const bool hasTarget = a.resolve.target != nullptr;
        const bool hasMode = a.resolve.mode != VK_RESOLVE_MODE_NONE;
        if (hasTarget != hasMode) {
            LOG("[vk] attachment %u declares a resolve %s\n", i,
                hasTarget ? "target with mode NONE" : "mode with no target");
            ok = false;
            continue;
        }
        if (!hasTarget) { continue; }

        // Resolving one sample into one sample is nothing to average
        // (VUID-VkRenderingAttachmentInfo-imageView-06861), and the destination is
        // where the averaging lands, so it is the single-sample one (06864).
        if (a.resource->samples == VK_SAMPLE_COUNT_1_BIT) {
            LOG("[vk] attachment %u resolves and is already 1-sample\n", i);
            ok = false;
        }
        if (a.resolve.target->samples != VK_SAMPLE_COUNT_1_BIT) {
            LOG("[vk] attachment %u resolves into a %d-sample image\n", i,
                static_cast<int>(a.resolve.target->samples));
            ok = false;
        }

        // VUID-VkRenderingAttachmentInfo-imageView-06865. Nothing converts on the way,
        // so the two are one format written twice until something derives one from the
        // other.
        if (a.resolve.target->format != a.resource->format) {
            LOG("[vk] attachment %u is format %d and resolves into format %d\n", i,
                static_cast<int>(a.resource->format),
                static_cast<int>(a.resolve.target->format));
            ok = false;
        }
    }

    // One image carries both aspects, so the two roles name one resource
    // (VUID-VkRenderingInfo-pDepthAttachment-06085) and one resolve destination (06086).
    //
    // **Asked of the descs, and asked again of the views in BeginPass.** Neither
    // answers the other: one desc is a different image every frame in flight, and two
    // descs can describe images nothing tells apart. The projection cannot help --
    // identity is the one thing it drops.
    if (depth != nullptr && stencil != nullptr) {
        if (depth->resource != stencil->resource) {
            LOG("[vk] the depth and stencil roles name two different images\n");
            ok = false;
        }
        if (depth->resolve.target != nullptr && stencil->resolve.target != nullptr
                && depth->resolve.target != stencil->resolve.target) {
            LOG("[vk] the depth and stencil roles resolve into two different images\n");
            ok = false;
        }
    }

    return ok;
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
        //
        // This is also what stands in for the frame half of 06085 -- the two views
        // being one view. It arrives with the layout above; until then no pass can
        // reach the case.
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

        // Covering the area, not matching it: an attachment may be larger, and
        // VUID-VkRenderingInfo-pNext-06079 and -06080 ask only that offset + extent
        // fits. Equality would refuse a pass drawing into part of its target, which is
        // what a render area is for.
        //
        // Here and not in ValidatePassDesc because the area is this frame's -- a resize
        // changes it without changing anything the pass declared.
        const int64_t needWidth = int64_t{area.offset.x} + area.extent.width;
        const int64_t needHeight = int64_t{area.offset.y} + area.extent.height;
        if (int64_t{got.extent.width} < needWidth
                || int64_t{got.extent.height} < needHeight) {
            LOG("[vk] attachment %u is %ux%u and the render area needs %lldx%lld\n", i,
                got.extent.width, got.extent.height,
                static_cast<long long>(needWidth), static_cast<long long>(needHeight));
            return false;
        }

        // The role the pass declared, not one read back out of the image. The layout
        // follows from it and is not a choice.
        const bool isColour = use.role == AttachmentRole::Color;

        // What drawing in this role needs to reach, against what the view exposes.
        //
        // **Asked, not taken.** The view settled what it shows and this pass settled
        // what it draws; either can be right while the pair is wrong, and a role whose
        // aspect the view does not carry would render into nothing.
        const VkImageAspectFlags roleAspect =
            isColour ? VK_IMAGE_ASPECT_COLOR_BIT
                     : (use.role == AttachmentRole::Depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                          : VK_IMAGE_ASPECT_STENCIL_BIT);
        if ((views[i]->view.aspect & roleAspect) == 0) {
            LOG("[vk] attachment %u is drawn as aspect 0x%x through a view that exposes"
                " 0x%x\n", i, roleAspect, views[i]->view.aspect);
            return false;
        }

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

            // The same comparison the attachment itself gets, for the same reason:
            // what the pass declared against what this frame handed over.
            const TextureDesc& wantInto = *use.resolve.target;
            const TextureDesc& gotInto = into->desc;
            if (gotInto.format != wantInto.format || gotInto.samples != wantInto.samples
                    || gotInto.usage != wantInto.usage) {
                LOG("[vk] attachment %u was handed a resolve image the pass did not"
                    " declare (format %d/%d)\n", i, static_cast<int>(gotInto.format),
                    static_cast<int>(wantInto.format));
                return false;
            }

            info.resolveMode = use.resolve.mode;
            info.resolveImageView = into->view.handle;
            info.resolveImageLayout = info.imageLayout;
        }

        // Where it has to be before the first draw. Skipped for LOAD, which reads what
        // came before and so has a writer to issue it instead.
        if (use.load != VK_ATTACHMENT_LOAD_OP_LOAD) {
            // Every aspect the image has, which is the barrier's own rule and not the
            // view's answer -- VUID-VkImageMemoryBarrier2-image-03320 wants both on a
            // combined format while a sampled view over the same image may carry only
            // one. A transition covers the image; a view is a window onto it.
            RecordAttachmentTransition(vk, cmd, views[i]->image.handle,
                                       FormatAspects(got.format), info, waitedStage);
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
