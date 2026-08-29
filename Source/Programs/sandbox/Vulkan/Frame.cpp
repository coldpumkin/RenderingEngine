#include "Vulkan/Frame.h"

bool CreateFrame(const VulkanDevice& dev, const Commands& commands, Frame* out) noexcept {
    out->dev = &dev;

    // PRIMARY: 큐에 직접 제출할 수 있다. SECONDARY는 다른 버퍼 안에서만 실행된다.
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

    // **신호된 상태로 만든다.** 첫 프레임엔 기다릴 이전 프레임이 없다.
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dev.table.vkCreateFence(dev.handle, &fenceInfo, nullptr, &out->inFlight) != VK_SUCCESS) {
        LOG("[vk] vkCreateFence(inFlight) failed\n");
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
    // cmd는 따로 반납하지 않는다 - 풀이 파괴될 때 같이 사라진다.
}

// ============================================================================
// 프레임 여닫기 - **동기화만 있고 그리는 것은 하나도 없다**
// ============================================================================

bool BeginFrame(const VulkanInstance& inst,
                const VulkanDevice& dev,
                Window* window,
                const Frame& frame,
                FrameTarget* out) noexcept {
    *out = FrameTarget{};

    // ---- 0. 그릴 곳 확보 ----
    // 리사이즈 통보는 창 콜백이 window에 직접 세워놨다.
    if (!EnsureSwapchain(inst, dev, window)) {
        return false;   // 최소화 중. 이번 프레임은 없다
    }
    Swapchain& swapchain = *window->swapchain;

    // ---- 1. 이전 프레임이 끝나기를 기다린다 (GPU -> CPU) ----
    //
    // **여기가 frames-in-flight의 값어치가 드러나는 자리다.** 1이면 바로 직전
    // 프레임을 기다리고, 2면 두 프레임 전 것을 기다린다 - 그동안 GPU가 앞선다.
    //
    // 실측(120Hz, 삼각형 하나): 여기서 쓰는 시간이 0.4%뿐이었다. 프레임 시간의 90%는
    // 아래 acquire(모니터 대기)에서 나온다. **GPU가 바빠져야 이 대기가 의미를 갖는다.**
    dev.table.vkWaitForFences(dev.handle, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

    // ---- 2. 이미지를 하나 빌린다 ----
    uint32_t imageIndex = 0;
    const VkResult acquired = dev.table.vkAcquireNextImageKHR(
        dev.handle, swapchain.handle, UINT64_MAX,
        frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);

    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        window->swapchainOutOfDate = true;
        return false;   // 다음 프레임 시작에 재생성된다
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        LOG("[vk] vkAcquireNextImageKHR failed (%d)\n", acquired);
        return false;
    }

    // ---- 3. 펜스 리셋 (**acquire가 성공한 뒤에**) ----
    // 먼저 리셋하면, acquire가 실패해 제출 없이 돌아가는 프레임에서 펜스가 영영
    // 신호되지 않고 다음 WaitForFences가 영원히 걸린다.
    dev.table.vkResetFences(dev.handle, 1, &frame.inFlight);

    out->image = &swapchain.images[imageIndex];
    out->extent = swapchain.extent;
    out->imageIndex = imageIndex;
    return true;
}

void EndFrame(const VulkanDevice& dev,
              Window* window,
              const Frame& frame,
              const FrameTarget& target) noexcept {
    // ---- 12. 제출 ----
    // acquire가 끝나야 이미지에 쓸 수 있고(wait), 다 쓰면 present가 알아야 한다(signal).
    // 기다리는 지점을 COLOR_ATTACHMENT_OUTPUT으로 좁히면 그 앞 스테이지는 미리 돈다.
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = frame.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = target.image->renderFinished;
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

    if (dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &submit, frame.inFlight)
            != VK_SUCCESS) {
        LOG("[vk] vkQueueSubmit2 failed\n");
        return;
    }

    // ---- 13. 화면에 내보낸다 ----
    const VkSwapchainKHR swapchainHandle = window->swapchain->handle;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &target.image->renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchainHandle;
    present.pImageIndices = &target.imageIndex;

    // SUBOPTIMAL은 에러가 아니다. 그려지긴 했고 다음 프레임에 다시 만들면 된다.
    const VkResult presented = dev.table.vkQueuePresentKHR(dev.queues.present, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        window->swapchainOutOfDate = true;
    }
}
