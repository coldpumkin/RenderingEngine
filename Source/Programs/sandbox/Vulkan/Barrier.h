#pragma once

// Image layout transition
// ============================================================================
//
// A GPU image is laid out differently depending on what is about to touch it, and a
// barrier is what moves it between those layouts. It carries memory visibility with
// the move -- whether an earlier write is readable by a later read -- which is why
// both halves take a stage and an access mask.
//
// The transitions a frame makes are not listed here. They live in the passes that
// issue them, each next to the reason it is needed, and a copy of that list here would
// be one more thing to keep in step -- it already went stale once, describing two
// passes when there were four.

#include "Vulkan/Core.h"

// Input:  cmd, image, aspect, src/dst stage·access, old/new layout
// Effect: appends one barrier to cmd
//
// Eight arguments, none of them defaulted. A default settles on the safe value --
// ALL_COMMANDS -- and synchronization gets quietly heavier with nothing reporting it.
// What to wait for and what to make visible is the caller's answer, every time.
// Output: the whole of an image with this aspect -- every level and every layer
//
// What almost every barrier here covers, and what the ones that do not are exactly the
// interesting case: baking a cube writes one layer at a time, and a barrier over all
// six would discard the faces already drawn.
inline VkImageSubresourceRange WholeImage(VkImageAspectFlags aspect) noexcept {
    return VkImageSubresourceRange{aspect, 0, VK_REMAINING_MIP_LEVELS,
                                   0, VK_REMAINING_ARRAY_LAYERS};
}

// Output: one array layer of an image, at one mip level
inline VkImageSubresourceRange OneLayer(VkImageAspectFlags aspect, uint32_t layer,
                                        uint32_t mip) noexcept {
    return VkImageSubresourceRange{aspect, mip, 1, layer, 1};
}

void RecordLayoutTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd, VkImage image,
                            const VkImageSubresourceRange& range,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                            VkImageLayout oldLayout, VkImageLayout newLayout) noexcept;


// The two ways a pass uses an image, as the two barriers they need
// ----------------------------------------------------------------------------
//
// A pass draws into some images and reads others, and those are not the same
// question. What has to be true before it runs differs, and so does which half of the
// barrier the pass can answer for itself:
//
//   draws into   the attachment already says it. loadOp is whether anything before
//                this matters, imageLayout is what it is about to become
//   reads        the destination is the reader's own; the source is whoever wrote it,
//                which a reader cannot know and a writer can
//
// Both still take the aspect. An Image does not carry the format it was made from, so
// nothing here can work it out -- that is the same gap AspectOf sits behind in
// Image.cpp, private to the file that has a format to ask.

// Effect: appends the barrier that puts an image where this attachment expects it
//
// loadOp CLEAR and DONT_CARE overwrite, so oldLayout is UNDEFINED and no earlier write
// has to be made visible. The destination comes from imageLayout: an attachment is
// either drawn into as colour or tested and written as depth, and the layout says
// which. Any other layout is refused rather than guessed at.
//
// waitedStage is what already waits on this image from outside this command buffer,
// and is TOP_OF_PIPE when nothing does. The swapchain image is the one that is not:
// SubmitFrame's semaphore waits at COLOR_ATTACHMENT_OUTPUT for it, and a transition
// scheduled ahead of that would run before the acquire.
//
// Contract: loadOp must not be LOAD. Loading reads what came before, and what wrote it
//           is not in the attachment -- that barrier is the frame's to issue.
void RecordAttachmentTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                                VkImage image, const VkImageSubresourceRange& range,
                                const VkRenderingAttachmentInfo& attachment,
                                VkPipelineStageFlags2 waitedStage) noexcept;

// Effect: appends the barrier that makes an image readable by a fragment stage
//
// The destination half is not taken, because every reader here is the same one: a
// sampler in a fragment stage. The source half is, and which pass passes it says
// something -- a pass publishing what it just wrote passes its own stage and layout,
// while a pass transitioning someone else's product passes facts it had to be told.
void RecordSampledTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                             VkImage image, const VkImageSubresourceRange& range,
                             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkImageLayout oldLayout) noexcept;
