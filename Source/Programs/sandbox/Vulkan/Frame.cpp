#include "Vulkan/Frame.h"

bool CreateFrame(const VulkanDevice& dev, const Commands& commands,
                 RenderTargetFormats formats, VkExtent2D extent,
                 Frame* out) noexcept {
    out->dev = &dev;

    // PRIMARY can be submitted to a queue directly. SECONDARY only runs inside
    // another buffer.
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

Frame::~Frame() {
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
                       const Frame& frame,
                       FrameTarget* out) noexcept {
    *out = FrameTarget{};

    // Skip, not Fatal. The loop already checked the window is drawable, but it
    // can change between that check and here (mid resize-drag), and treating
    // that as fatal kills the app while the user drags.
    //
    // The cost: if creation keeps failing at a non-zero size, this spins. Not a
    // normal path, so no retry counter yet.
    if (!EnsureSwapchain(dev, window)) {
        return FrameResult::Skip;
    }
    Swapchain& swapchain = *window->swapchain;

    // This is where frames-in-flight earns its keep: at 1 we wait on the previous
    // frame, at 2 on the one before that. Measured at 120Hz with one triangle it
    // costs 0.4% of the frame while 90% goes to the acquire below - the wait only
    // starts to matter once the GPU is busy.
    //
    // The return value matters even with an infinite timeout: the failures the
    // spec allows here are DEVICE_LOST and OOM, and after DEVICE_LOST this fence
    // is never signaled by anyone.
    const VkResult waited =
        dev.table.vkWaitForFences(dev.handle, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
    if (waited != VK_SUCCESS) {
        LOG("[vk] vkWaitForFences failed (%d) - the device may be lost\n", waited);
        return FrameResult::Fatal;
    }

    uint32_t imageIndex = 0;
    const VkResult acquired = dev.table.vkAcquireNextImageKHR(
        dev.handle, swapchain.handle, UINT64_MAX,
        frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);

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
    // draw is unrelated to acquire: acquire only decides where the result goes.
    out->draw = &frame.targets;
    out->drawResolveSet = frame.resolveSet;
    out->present = &swapchain.images[imageIndex];
    out->presentExtent = swapchain.extent;
    return FrameResult::Ready;
}

bool SubmitFrame(const VulkanDevice& dev,
                 const Frame& frame,
                 VkSemaphore signalWhenDone) noexcept {
    // Wait where the swapchain image is first touched. The present pass draws
    // into it directly, so that is COLOR_ATTACHMENT_OUTPUT. The scene pass only
    // touches our own images and can run before the acquire completes.
    //
    // This has to overlap RecordPresentPass's barrier srcStageMask - overlap, not
    // match. Checked with sync validation:
    //   wait=BLIT,           barrier=BLIT|COPY   -> 0 reports
    //   wait=BLIT|COLOR_OUT, barrier=BLIT        -> 0 reports
    //   wait=BLIT,           barrier=COPY        -> 20 reports (no overlap)
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = frame.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = signalWhenDone;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = frame.cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;

    // The reset sits immediately before the submit. Only a submit signals this
    // fence, and a submit only accepts an unsignaled one, so anything that can
    // fail in between leaves the fence unsignaled with nobody left to signal it -
    // and next round's vkWaitForFences(UINT64_MAX) never returns.
    //
    // The reset's result is checked too: submitting with a signaled fence is not
    // an error but invalid usage, so it turns into silent UB without the
    // validation layer.
    if (dev.table.vkResetFences(dev.handle, 1, &frame.inFlight) != VK_SUCCESS) {
        LOG("[vk] vkResetFences failed\n");
        return false;
    }

    // A failure here cannot be undone: there is no API to signal an ordinary
    // fence from the CPU (only timeline semaphores have one). It is OOM or
    // DEVICE_LOST anyway, so the loop ends.
    if (dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &submit, frame.inFlight)
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
    // Arrays, because several windows present at once. That is the axis present
    // grows on, and it is not the one submit grows on.
    present.swapchainCount = 1;
    present.pSwapchains = &swapchainHandle;
    present.pImageIndices = &image.index;

    const VkResult presented = dev.table.vkQueuePresentKHR(dev.queues.present, &present);

    // Either way the submit already happened and the fence will signal. What
    // splits the cases is whether a rebuild fixes it. These are only a size change.
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        window->swapchainOutOfDate = true;
        return true;
    }

    // Unrecoverable: DEVICE_LOST, SURFACE_LOST_KHR, OOM. A rebuild does not help
    // (after SURFACE_LOST, creating against that surface fails outright).
    //
    // Our policy is to say why and stop. Unreal retries even SURFACE_LOST up to
    // four times (DoCheckedSwapChainJob) because a shipping engine has to survive.
    if (presented != VK_SUCCESS) {
        LOG("[vk] vkQueuePresentKHR failed (%d)\n", presented);
        return false;
    }
    return true;
}
