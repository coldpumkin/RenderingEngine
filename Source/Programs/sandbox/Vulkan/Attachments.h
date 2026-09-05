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

    // Highest count both color and depth support, capped by kDesiredSampleCount.
    // 1 would mean no MSAA, which the resolve path does not handle (Config.h).
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// Output: the pipeline's view of the images a pass draws into
//
// **The one projection**, and the only way an AttachmentFormats is ever made.
//
// One list, not a colour array beside a depth pointer. What a target is for is in its
// usage -- COLOR_ATTACHMENT_BIT or DEPTH_STENCIL_ATTACHMENT_BIT -- so a caller
// separating them was restating by argument position what every desc already says.
// The projection drops usage, and which slot each format lands in was the one thing
// it dropped that the caller then had to hand back.
//
// Colour order is the list's order, because location 0, 1, 2 is a real order that the
// fragment stage relies on. Depth is found rather than placed: there is only one, so
// it has no position to be in.
//
// **The first null ends the list**, so how many targets there are is the list rather
// than a number written beside it. A gap is not expressible, on purpose -- a fragment
// stage's output locations have none either, and CheckOutputInterface refuses those.
//
// Contract: every desc must agree about samples. One rasterizationSamples covers a
//           whole pass, so no pipeline could honour two; a disagreement is logged and
//           the first one wins. At most one target may be a depth target, and a desc
//           that is neither is a mistake -- both are logged and skipped.
AttachmentFormats AttachmentFormatsOf(const TextureDesc* const targets[],
                                      uint32_t count) noexcept;

// What happens to one attachment over one pass, and every field of it is fixed as long
// as the pass means the same thing. The image it happens to is not: that changes with
// the frame in flight, so it is an argument to BeginPass.
//
// resolveMode is the line between the two. "This pass resolves" is what the pass is;
// "into that image" is this frame's.
struct AttachmentUse {
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE;
    VkClearValue clear{};                                    // read only when load is CLEAR
    VkResolveModeFlagBits resolve = VK_RESOLVE_MODE_NONE;    // NONE is no resolve
};

// One render pass instance, as far as it is settled before there is a frame.
//
// uses[] is positional with the views handed to BeginPass, the same way targets[] is
// positional in a GraphicsPipelineDesc -- and which of them is the depth one is read
// from each view's own usage, not from where it sits.
struct RenderPassDesc {
    AttachmentUse uses[kMaxColorTargets + 1]{};
    uint32_t useCount = 0;

    uint32_t layerCount = 1;
    uint32_t viewMask = 0;             // needs multiview, which we do not ask for
    VkRenderingFlags flags = 0;        // suspend/resume, for one instance across buffers
};

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
// views[i] is what uses[i] applies to, and resolves[i] is where uses[i].resolve sends
// it -- null wherever resolve is NONE. waitedStage is what already waits on these
// images from outside this command buffer, TOP_OF_PIPE when nothing does.
//
// Contract: count must equal desc.useCount. Refused rather than trusted, since a view
//           without its AttachmentUse would load and store by accident.
bool BeginPass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
               const RenderPassDesc& desc,
               const Texture* const views[], const Texture* const resolves[],
               uint32_t count, VkRect2D area,
               VkPipelineStageFlags2 waitedStage) noexcept;

// The comparison the pass creations make: the descs they were handed, projected, and
// what their pipeline actually baked. Nobody else can see both ends.
inline bool SameAttachmentFormats(const AttachmentFormats& a,
                                  const AttachmentFormats& b) noexcept {
    if (a.colorCount != b.colorCount || a.depth != b.depth || a.samples != b.samples) {
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
