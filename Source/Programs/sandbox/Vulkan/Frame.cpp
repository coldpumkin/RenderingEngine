#include "Vulkan/Frame.h"

bool CreateFrame(const VulkanDevice& dev, const Commands& commands,
                 VkFormat depthFormat, Frame* out) noexcept {
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

    // **그릴 곳은 창을 안 보고 만든다.** Config.h가 정한 고정 해상도다.
    // 스왑체인이 아직 없어도(최소화된 채로 실행) 여기는 성립한다 - 그게 요점이다.
    const VkExtent2D renderExtent{kRenderWidth, kRenderHeight};
    if (!CreateRenderTargets(dev, renderExtent, depthFormat, &out->targets)) {
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

FrameResult BeginFrame(const VulkanDevice& dev,
                       Window* window,
                       const Frame& frame,
                       FrameTarget* out) noexcept {
    *out = FrameTarget{};

    // ---- 0. 그릴 곳 확보 ----
    // 리사이즈 통보는 창 콜백이 window에 직접 세워놨다.
    //
    // **Skip이지 Fatal이 아니다.** 창이 그릴 수 있는 크기라는 건 루프가 이미 확인했지만,
    // 그 확인과 여기 사이에 창이 바뀔 수 있다 (리사이즈 드래그 중). 그 순간을 Fatal로
    // 보면 리사이즈 도중에 앱이 죽는다.
    // (대가: 크기가 0이 아닌데 생성이 계속 실패하면 여기서 돈다. 그건 정상 경로가
    //  아니고, 재시도 횟수를 세는 건 지금 필요한 복잡도가 아니다.)
    if (!EnsureSwapchain(dev, window)) {
        return FrameResult::Skip;
    }
    Swapchain& swapchain = *window->swapchain;

    // ---- 1. 이전 프레임이 끝나기를 기다린다 (GPU -> CPU) ----
    //
    // **여기가 frames-in-flight의 값어치가 드러나는 자리다.** 1이면 바로 직전
    // 프레임을 기다리고, 2면 두 프레임 전 것을 기다린다 - 그동안 GPU가 앞선다.
    //
    // 실측(120Hz, 삼각형 하나): 여기서 쓰는 시간이 0.4%뿐이었다. 프레임 시간의 90%는
    // 아래 acquire(모니터 대기)에서 나온다. **GPU가 바빠져야 이 대기가 의미를 갖는다.**
    //
    // **타임아웃이 UINT64_MAX인데도 반환값을 본다.** 스펙상 여기서 나올 수 있는 실패는
    // DEVICE_LOST와 메모리 부족이고, DEVICE_LOST면 이 펜스는 **영원히 신호되지 않는다**.
    // 그때 그냥 진행하면 아래 acquire도, 다음 순회의 이 대기도 계속 실패한다.
    const VkResult waited =
        dev.table.vkWaitForFences(dev.handle, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
    if (waited != VK_SUCCESS) {
        LOG("[vk] vkWaitForFences failed (%d) - 디바이스를 잃었을 수 있다\n", waited);
        return FrameResult::Fatal;
    }

    // ---- 2. 이미지를 하나 빌린다 ----
    uint32_t imageIndex = 0;
    const VkResult acquired = dev.table.vkAcquireNextImageKHR(
        dev.handle, swapchain.handle, UINT64_MAX,
        frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);

    // SUBOPTIMAL은 성공이다 - 이미지를 받았고 세마포어도 신호된다. 화질이 최적이 아닐 뿐.
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        window->swapchainOutOfDate = true;
        return FrameResult::Skip;   // 다음 프레임 시작에 재생성된다
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        // **스왑체인을 다시 만들어도 안 고쳐지는 것들이다**: DEVICE_LOST, SURFACE_LOST,
        // 메모리 부족. 여기서 Skip을 주면 다음 순회에 똑같이 실패하며 무한히 돈다.
        LOG("[vk] vkAcquireNextImageKHR failed (%d)\n", acquired);
        return FrameResult::Fatal;
    }

    // **펜스는 여기서 리셋하지 않는다.** 리셋의 짝은 acquire가 아니라 제출이다
    // (EndFrame 참고). 여기서 리셋하면 그 뒤에 실패할 수 있는 것이 남아 있다.

    // 그릴 곳은 acquire와 무관하다 - 프레임이 자기 것을 들고 있다.
    // acquire가 정하는 것은 **내보낼 곳**뿐이다.
    out->draw = &frame.targets;
    out->present = &swapchain.images[imageIndex];
    out->presentExtent = swapchain.extent;
    return FrameResult::Ready;
}

bool SubmitFrame(const VulkanDevice& dev,
                 const Frame& frame,
                 VkSemaphore signalWhenDone) noexcept {
    // acquire가 끝나야 이미지에 쓸 수 있고(wait), 다 쓰면 present가 알아야 한다(signal).
    //
    // **대기 지점은 스왑체인 이미지를 처음 만지는 곳이다.** 오프스크린이 되면서 그게
    // 색 첨부 쓰기가 아니라 블릿으로 바뀌었다. 앞의 렌더링은 우리 이미지에만 그리므로
    // acquire를 안 기다려도 안전하다.
    // (이론상 그만큼 겹쳐 돌 수 있지만 **재보지 않았다.** FIFO에 프레임 시간의 92%가
    //  acquire 대기라 지금 구성으로는 관측이 안 된다.)
    //
    // 이 값은 RecordFrame의 "스왑체인 -> TRANSFER_DST" 배리어 srcStageMask와
    // **겹쳐야 한다.** 같을 필요는 없다 - 동기화 검증으로 확인했다:
    //   wait=BLIT,           barrier=BLIT|COPY   -> 0건
    //   wait=BLIT|COLOR_OUT, barrier=BLIT        -> 0건
    //   wait=BLIT,           barrier=COPY        -> **20건** (겹치는 게 없다)
    // 원래 버그였던 barrier=TOP_OF_PIPE가 마지막 경우다.
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = frame.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;

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

    // **펜스 리셋은 제출 바로 앞이다.**
    //
    // 펜스를 신호하는 것은 제출뿐이고 제출은 비신호 펜스만 받는다. 그래서 리셋과 제출
    // 사이에 실패할 수 있는 것이 끼면, 그 프레임의 펜스는 비신호로 남고 신호할 사람이
    // 없어진다 - 다음 순회의 vkWaitForFences(UINT64_MAX)가 영원히 걸린다.
    //
    // 리셋 실패도 봐야 한다. 신호된 펜스로 제출하는 것은 에러가 아니라 **무효 사용**이라
    // 검증 레이어를 끄면 조용히 UB가 된다.
    if (dev.table.vkResetFences(dev.handle, 1, &frame.inFlight) != VK_SUCCESS) {
        LOG("[vk] vkResetFences failed\n");
        return false;
    }

    // 실패하면 못 되돌린다 - 일반 펜스를 CPU에서 신호하는 API가 없다(타임라인
    // 세마포어에만 있다). 어차피 OUT_OF_MEMORY / DEVICE_LOST뿐이라 루프를 끝낸다.
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
    // **배열인 것에 주목.** 창이 여럿이면 한 번에 내보낸다 - present가 늘어나는 축이
    // 창 개수라는 뜻이고, 제출(큐 개수)과 다르다.
    present.swapchainCount = 1;
    present.pSwapchains = &swapchainHandle;
    present.pImageIndices = &image.index;

    const VkResult presented = dev.table.vkQueuePresentKHR(dev.queues.present, &present);

    // 어느 쪽이든 **제출은 이미 됐고 펜스는 신호된다.** 다음 프레임이 대기에 걸릴
    // 걱정은 없다. 갈리는 건 **다시 만들면 고쳐지는가**다.
    //
    // 회복 가능: 창 크기가 달라졌을 뿐이다. 다음 프레임 시작에 다시 만든다.
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        window->swapchainOutOfDate = true;
        return true;
    }

    // 회복 불가: DEVICE_LOST, SURFACE_LOST_KHR, 메모리 부족 등. 다시 만들어도 안 고쳐진다
    // (SURFACE_LOST면 그 서피스로는 생성 자체가 실패한다).
    //
    // **언리얼은 SURFACE_LOST도 재생성으로 4번까지 시도한다**(DoCheckedSwapChainJob).
    // 출하 엔진은 어떻게든 살아남아야 하기 때문이고, 우리는 이유를 말하고 끝내는 쪽을
    // 골랐다 - 몰라서가 아니라 골라서다.
    if (presented != VK_SUCCESS) {
        LOG("[vk] vkQueuePresentKHR failed (%d)\n", presented);
        return false;
    }
    return true;
}
