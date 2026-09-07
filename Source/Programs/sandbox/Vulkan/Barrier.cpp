#include "Vulkan/Barrier.h"

void RecordLayoutTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd, VkImage image,
                            const VkImageSubresourceRange& range,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                            VkImageLayout oldLayout, VkImageLayout newLayout) noexcept {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    // IGNORED: no queue family transfer. Everything we barrier lives on the graphics
    // queue, and a transfer would need the matching pair on the other side.
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vk.vkCmdPipelineBarrier2(cmd, &dep);
}

void RecordAttachmentTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                                VkImage image, const VkImageSubresourceRange& range,
                                const VkRenderingAttachmentInfo& attachment,
                                VkPipelineStageFlags2 waitedStage) noexcept {
    if (attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
        LOG("[vk] an attachment that loads needs the barrier its writer's frame issues\n");
        return;
    }

    VkPipelineStageFlags2 dstStage = 0;
    VkAccessFlags2 dstAccess = 0;
    switch (attachment.imageLayout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            dstStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            // Both halves of the test, because the driver picks which one runs.
            dstStage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                     | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        default:
            LOG("[vk] layout %d is not one an attachment is drawn into here\n",
                static_cast<int>(attachment.imageLayout));
            return;
    }

    // srcAccess 0 with any waitedStage: nothing written before this is read after it.
    // UNDEFINED discards the contents, which is what makes that true.
    RecordLayoutTransition(vk, cmd, image, range,
                           waitedStage, 0,
                           dstStage, dstAccess,
                           VK_IMAGE_LAYOUT_UNDEFINED, attachment.imageLayout);
}

void RecordSampledTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                             VkImage image, const VkImageSubresourceRange& range,
                             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkImageLayout oldLayout) noexcept {
    RecordLayoutTransition(vk, cmd, image, range,
                           srcStage, srcAccess,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
