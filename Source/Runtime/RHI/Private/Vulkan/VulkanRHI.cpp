#include "VulkanRHI.h"

#include "RHILog.h"
#include "VulkanPhysicalDevice.h"
#include "Implementations/VulkanViewport.h"
#include "NativeWrappers/VulkanCommandPool.h"
#include "VulkanResult.h"

#include <utility>

namespace LambdaEngine {

std::unique_ptr<VulkanRHI> VulkanRHI::Create() noexcept {
    // GPU backend만 만든다. 창이 하나도 없어도 여기까지는 성립해야 한다.
    //   인스턴스 -> GPU 선택 -> 디바이스. 그리는 데 쓰는 자원은 뷰포트가 가진다.
    auto instance = VulkanInstance::Create();
    if (instance == nullptr) {
        return nullptr; // 실패 이유는 각 단계가 이미 기록했다
    }

    // 고르는 일과 만드는 일을 나눈다. 선택은 인스턴스 레벨 작업이고 엔진 정책이다.
    const PhysicalDeviceSelection selection = SelectPhysicalDevice(*instance);
    if (selection.device == VK_NULL_HANDLE) {
        return nullptr;
    }

    auto device = VulkanDevice::Create(*instance, selection);
    if (device == nullptr) {
        return nullptr;
    }

    LAMBDA_LOG_INFO("GPU backend ready");

    return std::unique_ptr<VulkanRHI>(
        new VulkanRHI(std::move(instance), std::move(device)));
}

VulkanRHI::VulkanRHI(std::unique_ptr<VulkanInstance> instance,
                     std::unique_ptr<VulkanDevice> device) noexcept
    : instance_(std::move(instance)), device_(std::move(device)) {}

bool VulkanRHI::EnsureDrawingResources() noexcept {
    if (context_ != nullptr) {
        return true;
    }

    // 커맨드 풀: 큐 패밀리에 묶이고 디바이스와 함께 죽어야 하므로 **소유는 디바이스**다.
    // 만드는 것이 여기인 이유는 위 선언의 주석 참고 (소유와 생성은 다른 축이다).
    auto pool = VulkanCommandPool::Create(*device_);
    if (pool == nullptr) {
        return false;
    }
    device_->SetCommandPool(std::move(pool));

    // 기록기: 펜스·세마포어 개수가 frames-in-flight에 묶이므로 디바이스가 아니라 여기다.
    context_ = VulkanCommandContext::Create(*device_);
    return context_ != nullptr;
}

RHICommandContext* VulkanRHI::BeginDrawingViewport(RHIViewport& viewport) noexcept {
    if (!EnsureDrawingResources()) {
        return nullptr;
    }

    auto& target = static_cast<VulkanViewport&>(viewport);

    // ---- 0. 그릴 곳을 확보한다 ----
    // 낡았거나 없으면 다시 만든다. oldSwapchain을 넘기는 순간 이전 것은 은퇴한다 -
    // 한 서피스에 스왑체인 둘이 동시에 존재할 수 없어서 안 넘기면 생성 자체가 실패한다.
    if (target.IsOutOfDate() || target.Swapchain() == nullptr) {
        device_->WaitIdle();

        const VulkanSwapchain* current = target.Swapchain();
        const VkSwapchainKHR retiring =
            current != nullptr ? current->Handle() : VK_NULL_HANDLE;

        auto fresh = VulkanSwapchain::Create(*device_, target.Surface(), retiring);
        target.ClearOutOfDate();
        target.SetSwapchain(std::move(fresh));   // nullptr이면 "그릴 곳 없음"
    }

    const VulkanSwapchain* swapchain = target.Swapchain();
    if (swapchain == nullptr) {
        return nullptr;   // 최소화 중. 실패가 아니라 "이번 프레임은 없다"
    }

    // ---- 1. 이전 프레임이 끝나기를 기다린다 (GPU -> CPU) ----
    context_->WaitForPreviousFrame();

    // ---- 2. 이미지를 하나 빌린다 ----
    uint32_t imageIndex = 0;
    const VkResult acquired = device_->Table().vkAcquireNextImageKHR(
        device_->Handle(), swapchain->Handle(), UINT64_MAX,
        context_->ImageAvailable(), VK_NULL_HANDLE, &imageIndex);

    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        target.MarkOutOfDate();
        return nullptr;   // 다음 프레임 시작에 재생성된다
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        LAMBDA_LOG_ERROR("vkAcquireNextImageKHR failed: %s", ToString(acquired));
        return nullptr;
    }
    target.SetAcquiredIndex(imageIndex);

    // ---- 3. 펜스 리셋 (acquire가 성공한 뒤에) ----
    // 먼저 리셋하면, acquire가 실패해 제출 없이 돌아가는 프레임에서 펜스가 영영 신호되지
    // 않고 다음 WaitForFences가 걸린다.
    context_->ResetFence();

    // ---- 4~5. 기록 시작 ----
    context_->BeginRecording();

    return context_.get();
}

void VulkanRHI::EndDrawingViewport(RHIViewport& viewport) noexcept {
    auto& target = static_cast<VulkanViewport&>(viewport);
    const SwapchainImage& image =
        target.Swapchain()->ImageAt(target.AcquiredIndex());

    // ---- 10~11. present 레이아웃으로 바꾸고 기록을 닫는다 ----
    context_->EndRecordingForPresent(target);

    const VolkDeviceTable& vk = device_->Table();
    const VkQueue queue = device_->GraphicsQueue();

    // ---- 12. 제출 ----
    // acquire가 끝나야 이미지에 쓸 수 있고(wait), 다 쓰면 present가 알아야 한다(signal).
    // 기다리는 지점을 COLOR_ATTACHMENT_OUTPUT으로 좁히면 그 앞 스테이지는 미리 돈다.
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = context_->ImageAvailable();
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = image.renderFinished;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = context_->Buffer();

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;

    const VkResult submitted = vk.vkQueueSubmit2(queue, 1, &submit, context_->InFlight());
    if (submitted != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkQueueSubmit2 failed: %s", ToString(submitted));
        return;
    }

    // ---- 13. 화면에 내보낸다 ----
    const VkSwapchainKHR swapchainHandle = target.Swapchain()->Handle();
    const uint32_t imageIndex = target.AcquiredIndex();

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &image.renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchainHandle;
    present.pImageIndices = &imageIndex;

    // SUBOPTIMAL은 에러가 아니다. 그려지긴 했고 다음 프레임에 다시 만들면 된다.
    const VkResult presented = vk.vkQueuePresentKHR(queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        target.MarkOutOfDate();
    }
}

std::unique_ptr<RHIViewport> VulkanRHI::CreateViewport(NativeWindowHandle window) noexcept {
    // 소유권을 넘긴다. 뷰포트는 창의 수명을 따르므로 RHI가 들고 있으면 불변식이 섞인다 (D95).
    return VulkanViewport::Create(*instance_, *device_, window);
}

void VulkanRHI::ResizeViewport(RHIViewport& viewport) noexcept {
    // 표시만 한다. 창 콜백은 아무 때나 오고 그 시점에 GPU가 프레임 중일 수 있다.
    static_cast<VulkanViewport&>(viewport).MarkOutOfDate();
}

std::unique_ptr<DynamicRHI> CreateRHI() noexcept {
    return VulkanRHI::Create();
}

} // namespace LambdaEngine
