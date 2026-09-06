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

// Vulkan's own ceiling is higher; this is what a G-buffer needs and nothing here has
// ever wanted more.
inline constexpr uint32_t kMaxColorTargets = 4;

// Three roles, and a role is declared rather than discovered
//
// A pass decides what it draws here; a format and a usage bit are independent facts
// that say whether that decision can be honoured. Read back out of usage the two were
// one thing and nothing was left over to check.
//
// Depth and Stencil are separate roles over one image, which is Vulkan's shape:
// VkRenderingInfo carries pDepthAttachment and pStencilAttachment with a loadOp and a
// storeOp each, and VUID-VkRenderingInfo-pDepthAttachment-06085 requires their views to
// be the same one when both are used. A combined format cannot say which was meant, so
// here the inference is not merely lossy -- there is no answer to infer.
enum class AttachmentRole { Color, Depth, Stencil };

// Where a resolve goes, and how. Declared together because neither means anything
// alone, and NONE is the default so a pass that resolves has to say so.
//
// target is the logical destination, fixed for as long as the pass means the same
// thing. Which image that is this frame is BeginPass's resolves[] -- the same split
// the attachment itself is under.
//
// Contract: mode is not NONE exactly when target is not null.
struct ResolveUse {
    const TextureDesc* target = nullptr;
    VkResolveModeFlagBits mode = VK_RESOLVE_MODE_NONE;
};

// One attachment: which image, what this pass uses it as, and what happens to it.
//
// One struct rather than two arrays walked in step. The pair that had drifted furthest
// apart was the resolve -- the mode was declared and the destination only arrived as a
// record-time argument, so there was no declaration to check it against.
//
// resource is the logical image; the first null one ends the list. This frame's image
// for it is BeginPass's views[].
struct Attachment {
    const TextureDesc* resource = nullptr;
    AttachmentRole role = AttachmentRole::Color;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE;
    VkClearValue clear{};                 // read only when load is CLEAR
    ResolveUse resolve;
};

// Every colour target plus depth and stencil, which are two entries over one image.
inline constexpr uint32_t kMaxAttachments = kMaxColorTargets + 2;

// **What one pipeline baked, and nothing a caller writes.** Every field is read off
// the TextureDescs handed to CreateGraphicsPipeline, which is the description the
// images themselves are made from.
//
// It is those descs minus their extent, and that omission is the whole reason this
// type exists rather than the descs being kept. A resize remakes the images at a new
// size and does not rebuild any pipeline -- ResizeScenePass -- so an extent stored
// here would be right when it was written and wrong from the first resize on.
// Nothing else about a target moves, which is why nothing else is dropped.
//
// samples is one value and not one per attachment because Vulkan has one
// rasterizationSamples for a whole pass. AttachmentFormatsOf is where the descs are
// checked for agreeing about it.
struct AttachmentFormats {
    // colorCount entries, and the fragment stage decides how many: it declares the
    // outputs, and CreateGraphicsPipeline refuses a pipeline whose target count says
    // something else. A depth-only pass leaves this empty.
    VkFormat color[kMaxColorTargets]{};
    uint32_t colorCount = 0;

    // UNDEFINED means no depth. No shader says so -- depth is fixed-function -- so
    // unlike the field above, this one really is the pass's to choose.
    VkFormat depth = VK_FORMAT_UNDEFINED;

    // And whether stencil is written, which is a separate decision over the same image.
    // Declared, not read out of the depth format: derived, a device whose depth format
    // came back D32_SFLOAT_S8_UINT would compile every pipeline claiming a stencil
    // attachment no pass ever begins -- VUID-vkCmdDraw-dynamicRenderingUnusedAttachments
    // -08916, at every draw.
    //
    // Contract: when both are set they must be equal; one image carries both
    //           (VUID-VkGraphicsPipelineCreateInfo-renderPass-06589).
    VkFormat stencil = VK_FORMAT_UNDEFINED;

    // Highest count both color and depth support, capped by kDesiredSampleCount.
    // 1 would mean no MSAA, which the resolve path does not handle (Config.h).
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// Output: the pipeline's view of the images a pass draws into
//
// **The one projection**, and the only way an AttachmentFormats is ever made.
//
// One list, and each entry's role says which slot its format lands in. Colour order is
// the list's order, because location 0, 1, 2 is an order the fragment stage relies on;
// depth and stencil have one slot each and so no position to be in.
//
// **The first null resource ends the list**, so how many there are is the list rather
// than a number written beside it. A gap is not expressible, on purpose -- a fragment
// stage's output locations have none either, and CheckOutputInterface refuses those.
//
// The role is not worked out here. What is done here is refusing one the desc cannot
// honour: a depth role over a colour format, or an image not made to be drawn into.
//
// Contract: every desc must agree about samples. One rasterizationSamples covers a
//           whole pass, so no pipeline could honour two; a disagreement is logged and
//           the first one wins. At most one entry each may be depth or stencil.
AttachmentFormats AttachmentFormatsOf(const Attachment attachments[],
                                      uint32_t count) noexcept;

// Output: the same contract, with the roles said by which argument a desc arrives as
//
// For a caller that has no attachments to point at -- a pipeline is compiled against a
// contract and has no pass to take one from. Position is the declaration here: colour
// order is the array's order, and depth is null for a pass that has none.
AttachmentFormats AttachmentFormatsFor(const TextureDesc* const colour[],
                                       uint32_t colourCount,
                                       const TextureDesc* depth) noexcept;

// One render pass instance, as far as it is settled before there is a frame.
//
// **The whole of what this pass intends, and nothing of this frame.** Everything here
// is true for every frame in flight; the images are not, so they arrive at BeginPass.
// Projected, it is the same contract a GraphicsPipelineDesc holds, which is what lets a
// pass and its pipeline be compared with one call.
struct RenderPassDesc {
    Attachment attachments[kMaxAttachments]{};

    uint32_t layerCount = 1;
    uint32_t viewMask = 0;             // needs multiview, which we do not ask for
    VkRenderingFlags flags = 0;        // suspend/resume, for one instance across buffers
};

// Output: what a pass draws into, in the form a pipeline bakes. The one comparison a
//         pass creation makes against its pipeline.
inline AttachmentFormats PassFormats(const RenderPassDesc& desc) noexcept {
    return AttachmentFormatsOf(desc.attachments, kMaxAttachments);
}

// Effect: puts every attachment where it is about to be used, then begins the render
//         pass instance desc describes over these views.
//
// The barriers come from here because their two inputs do: a loadOp that overwrites
// makes the old contents dead, and the layout follows from what the image is. Written
// by hand they were the same two values read a second time, in another file.
//
// An attachment whose loadOp is LOAD gets none. Loading reads what came before, and
// what wrote it is not in this call -- that barrier belongs to whoever wrote it.
//
// views[i] is this frame's image for attachments[i].resource, and resolves[i] is this
// frame's image for attachments[i].resolve.target -- null wherever there is none.
// waitedStage is what already waits on these images from outside this command buffer,
// TOP_OF_PIPE when nothing does.
//
// How many there are comes from desc, not from an argument. Each view is checked
// against the desc it is standing in for, so handing them over in the wrong order is a
// refusal wherever the two descs differ rather than a picture with two images swapped.
bool BeginPass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
               const RenderPassDesc& desc,
               const Texture* const views[], const Texture* const resolves[],
               VkRect2D area, VkPipelineStageFlags2 waitedStage) noexcept;

// The comparison the pass creations make: the descs they were handed, projected, and
// what their pipeline actually baked. Nobody else can see both ends.
inline bool SameAttachmentFormats(const AttachmentFormats& a,
                                  const AttachmentFormats& b) noexcept {
    if (a.colorCount != b.colorCount || a.depth != b.depth || a.stencil != b.stencil
            || a.samples != b.samples) {
        return false;
    }
    for (uint32_t i = 0; i < a.colorCount; ++i) {
        if (a.color[i] != b.color[i]) { return false; }
    }
    return true;
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
