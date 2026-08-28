#include "Implementations/VulkanCommandContext.h"

#include "Implementations/VulkanViewport.h"
#include "NativeWrappers/VulkanCommandPool.h"

#include "RHILog.h"
#include "VulkanResult.h"

namespace LambdaEngine {
namespace {

// 이미지 레이아웃 전이. 프레임에 두 번 나오는데 방향만 다르다.
//
// synchronization2를 쓰는 이유: 스테이지와 액세스 마스크를 배리어마다 따로 줄 수 있어서
// "무엇을 기다리는가"가 배리어 자체에 적힌다. 예전 API는 그것이 함수 인자로 나가 있었다.
void RecordLayoutTransition(const VolkDeviceTable& vk,
                            VkCommandBuffer cmd,
                            VkImage image,
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
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vk.vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

std::unique_ptr<VulkanCommandContext> VulkanCommandContext::Create(
    const VulkanDevice& device) noexcept {

    const VolkDeviceTable& vk = device.Table();
    const VkDevice handle = device.Handle();

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    const VkResult semaphoreResult =
        vk.vkCreateSemaphore(handle, &semaphoreInfo, nullptr, &imageAvailable);
    if (semaphoreResult != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkCreateSemaphore failed: %s", ToString(semaphoreResult));
        return nullptr;
    }

    // 신호된 상태로 시작한다. 첫 프레임의 WaitForFences가 걸리지 않아야 한다.
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence inFlight = VK_NULL_HANDLE;
    const VkResult fenceResult = vk.vkCreateFence(handle, &fenceInfo, nullptr, &inFlight);
    if (fenceResult != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkCreateFence failed: %s", ToString(fenceResult));
        vk.vkDestroySemaphore(handle, imageAvailable, nullptr);
        return nullptr;
    }

    LAMBDA_LOG_INFO("command context ready");

    return std::unique_ptr<VulkanCommandContext>(
        new VulkanCommandContext(device, imageAvailable, inFlight));
}

VulkanCommandContext::VulkanCommandContext(const VulkanDevice& device,
                                           VkSemaphore imageAvailable,
                                           VkFence inFlight) noexcept
    : device_(device), imageAvailable_(imageAvailable), inFlight_(inFlight) {}

VulkanCommandContext::~VulkanCommandContext() {
    // 제출된 작업이 이 펜스와 세마포어를 아직 쓰고 있을 수 있다. 자기 자원은 자기가
    // 안전하게 파괴한다 - 소유자가 대신 기다려주기를 기대하지 않는다.
    device_.WaitIdle();

    const VolkDeviceTable& vk = device_.Table();
    vk.vkDestroyFence(device_.Handle(), inFlight_, nullptr);
    vk.vkDestroySemaphore(device_.Handle(), imageAvailable_, nullptr);

    LAMBDA_LOG_INFO("command context destroyed");
}

VkCommandBuffer VulkanCommandContext::Buffer() const noexcept {
    return device_.CommandPool().Buffer();
}

void VulkanCommandContext::WaitForPreviousFrame() const noexcept {
    device_.Table().vkWaitForFences(device_.Handle(), 1, &inFlight_, VK_TRUE, UINT64_MAX);
}

void VulkanCommandContext::ResetFence() const noexcept {
    device_.Table().vkResetFences(device_.Handle(), 1, &inFlight_);
}

void VulkanCommandContext::BeginRecording() const noexcept {
    const VulkanCommandPool& pool = device_.CommandPool();
    pool.Reset();
    pool.Begin();
}

void VulkanCommandContext::BeginRenderPass(RHIViewport& target) noexcept {
    // 상위가 준 것은 RHIViewport다. 우리 백엔드의 것임은 우리가 안다 - 다른 백엔드의
    // 뷰포트가 여기 올 수 있으려면 두 백엔드가 동시에 살아야 하는데 그런 구성이 없다.
    const auto& viewport = static_cast<const VulkanViewport&>(target);
    const VulkanSwapchain* swapchain = viewport.Swapchain();
    const SwapchainImage& image = swapchain->ImageAt(viewport.AcquiredIndex());

    const VolkDeviceTable& vk = device_.Table();
    const VkCommandBuffer cmd = Buffer();

    // 그릴 수 있는 레이아웃으로. oldLayout이 UNDEFINED인 것은 이전 내용을 안 쓰기
    // 때문이다 - 어차피 loadOp=CLEAR로 덮는다. 보존을 요구하면 드라이버가 실제로
    // 복사를 해야 한다.
    RecordLayoutTransition(vk, cmd, image.image,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // 다이나믹 렌더링: VkRenderPass/VkFramebuffer 객체를 미리 만들지 않는다.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = image.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // [임시] 클리어 값이 여기 박혀 있다. 언리얼은 렌더 타겟 리소스가 들고 렌더패스는
    // 클리어 여부만 정한다. 텍스처 타입이 생기면 이 자리가 바뀐다.
    color.clearValue.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = swapchain->Extent();
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vk.vkCmdBeginRendering(cmd, &rendering);
}

void VulkanCommandContext::EndRenderPass() noexcept {
    device_.Table().vkCmdEndRendering(Buffer());
}

void VulkanCommandContext::EndRecordingForPresent(const VulkanViewport& target) noexcept {
    const SwapchainImage& image = target.Swapchain()->ImageAt(target.AcquiredIndex());

    RecordLayoutTransition(device_.Table(), Buffer(), image.image,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    device_.CommandPool().End();
}

} // namespace LambdaEngine
