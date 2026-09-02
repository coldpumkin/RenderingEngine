#include "Vulkan/Frame.h"

bool CreateFrameSlot(const VulkanDevice& dev, const Commands& commands,
                 RenderTargetFormats formats, VkExtent2D extent,
                 FrameSlot* out) noexcept {
    out->dev = &dev;

    // PRIMARY submits to a queue directly; SECONDARY only runs inside another.
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commands.graphics;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (dev.table.vkAllocateCommandBuffers(dev.handle, &allocInfo, &out->cmd) != VK_SUCCESS) {
        LOG("[vk] vkAllocateCommandBuffers failed\n");
        return false;
    }

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (dev.table.vkCreateSemaphore(dev.handle, &semaphoreInfo, nullptr, &out->imageAvailable)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateSemaphore(imageAvailable) failed\n");
        return false;
    }

    // Created signaled: the first frame has no previous submit to wait for.
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dev.table.vkCreateFence(dev.handle, &fenceInfo, nullptr, &out->inFlight) != VK_SUCCESS) {
        LOG("[vk] vkCreateFence(inFlight) failed\n");
        return false;
    }

    // Built without looking at the window, so this works while minimized - there
    // may be no swapchain yet.
    if (!CreateRenderTargets(dev, extent, formats, &out->targets)) {
        return false;
    }
    return true;
}

FrameSlot::~FrameSlot() {
    if (dev == nullptr) { return; }
    if (inFlight != VK_NULL_HANDLE) {
        dev->table.vkDestroyFence(dev->handle, inFlight, nullptr);
    }
    if (imageAvailable != VK_NULL_HANDLE) {
        dev->table.vkDestroySemaphore(dev->handle, imageAvailable, nullptr);
    }
    // cmd is not returned: the pool frees it.
}

// Opening and closing. Synchronization only - nothing here draws.

FrameResult BeginFrame(const VulkanDevice& dev,
                       Window* window,
                       const FrameSlot& slot,
                       AcquiredFrame* out) noexcept {
    *out = AcquiredFrame{};

    // Skip, not Fatal: the window can stop being drawable between the loop's check
    // and here, mid resize-drag. The cost is a spin if creation keeps failing.
    if (!EnsureSwapchain(dev, window)) {
        return FrameResult::Skip;
    }
    Swapchain& swapchain = *window->swapchain;

    // At 1 slot we wait on the previous frame, at 2 on the one before. Measured at
    // 120Hz: 0.4% of the frame, against 90% in the acquire below.
    //
    // The result matters despite the infinite timeout: after DEVICE_LOST nobody
    // signals this fence.
    const VkResult waited =
        dev.table.vkWaitForFences(dev.handle, 1, &slot.inFlight, VK_TRUE, UINT64_MAX);
    if (waited != VK_SUCCESS) {
        LOG("[vk] vkWaitForFences failed (%d) - the device may be lost\n", waited);
        return FrameResult::Fatal;
    }

    uint32_t imageIndex = 0;
    const VkResult acquired = dev.table.vkAcquireNextImageKHR(
        dev.handle, swapchain.handle, UINT64_MAX,
        slot.imageAvailable, VK_NULL_HANDLE, &imageIndex);

    // SUBOPTIMAL counts as success: an image came back and the semaphore will signal.
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        window->swapchainOutOfDate = true;
        return FrameResult::Skip;   // rebuilt at the start of the next frame
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        // DEVICE_LOST, SURFACE_LOST and OOM do not survive a rebuild. Returning
        // Skip would fail identically next round, forever.
        LOG("[vk] vkAcquireNextImageKHR failed (%d)\n", acquired);
        return FrameResult::Fatal;
    }

    // The fence is not reset here - reset pairs with submit (see SubmitFrame).
    out->slot = &slot;
    out->image = &swapchain.images[imageIndex];
    out->extent = swapchain.extent;
    return FrameResult::Ready;
}

bool SubmitFrame(const VulkanDevice& dev, const AcquiredFrame& acquired) noexcept {
    const FrameSlot& slot = *acquired.slot;
    // Wait where the swapchain image is first touched -- the present pass draws
    // into it. The scene pass may run before the acquire completes.
    //
    // Must overlap RecordPresentPass's barrier srcStageMask, not match it. Checked
    // with sync validation: no overlap gave 20 reports, partial overlap gave 0.
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = slot.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = acquired.image->renderFinished;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = slot.cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;

    // Reset immediately before the submit: only a submit signals this fence, so
    // anything failing in between would leave next round's wait hanging forever.
    // The reset's result is checked because a signaled fence at submit is silent UB.
    if (dev.table.vkResetFences(dev.handle, 1, &slot.inFlight) != VK_SUCCESS) {
        LOG("[vk] vkResetFences failed\n");
        return false;
    }

    // Not undoable: the CPU cannot signal an ordinary fence. OOM or DEVICE_LOST
    // anyway, so the loop ends.
    if (dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &submit, slot.inFlight)
            != VK_SUCCESS) {
        LOG("[vk] vkQueueSubmit2 failed\n");
        return false;
    }
    return true;
}

bool PresentFrame(const VulkanDevice& dev,
                  Window* window,
                  const SwapchainImage& image) noexcept {
    const VkSwapchainKHR swapchainHandle = window->swapchain->handle;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &image.renderFinished;
    // Arrays: several windows present at once. Submit does not grow on that axis.
    present.swapchainCount = 1;
    present.pSwapchains = &swapchainHandle;
    present.pImageIndices = &image.index;

    const VkResult presented = dev.table.vkQueuePresentKHR(dev.queues.present, &present);

    // The submit already happened and the fence will signal. A rebuild fixes these.
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        window->swapchainOutOfDate = true;
        return true;
    }

    // Unrecoverable: a rebuild does not help after SURFACE_LOST. We say why and
    // stop; Unreal retries four times (DoCheckedSwapChainJob) because it must ship.
    if (presented != VK_SUCCESS) {
        LOG("[vk] vkQueuePresentKHR failed (%d)\n", presented);
        return false;
    }
    return true;
}
