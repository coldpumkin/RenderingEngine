#pragma once

#include "RHICommandContext.h"

#include "NativeWrappers/VulkanDevice.h"

#include <volk.h>

#include <memory>

namespace LambdaEngine {

class VulkanViewport;

// RHICommandContext의 Vulkan 구현. **한 프레임을 기록하고 제출하는 데 필요한 것 한 벌.**
//
// ---------------------------------------------------------------------------
// 무엇을 드나 (기준 A)
//
//   ① 소유    inFlight_ 펜스, imageAvailable_ 세마포어
//   ② 파괴에  device_ (전부 디바이스 레벨 함수로 만들고 파괴한다)
//
// **커맨드 풀은 소유하지 않는다.** 풀은 디바이스가 소유하고 여기서는 `device_.CommandPool()`로
// 빌려 쓴다 - 풀은 디바이스 수명이고 이 둘은 frames-in-flight 수명이라 묶이지 않는다.
// 언리얼도 같다: `FVulkanCommandListContext`는 풀을 `FVulkanCommandBufferPool&`로 참조한다.
//
// **왜 이 둘이 여기 있나**: 개수가 전부 frames-in-flight에 묶인다. 커맨드 버퍼도 그렇고,
// "기록 -> 제출 -> 그게 끝나기를 기다림"이 한 덩어리로 돈다. 지금은 1이라 한 벌이다.
//
// **acquire 세마포어를 이미지당으로 둘 수 없는 이유**: acquire를 부르기 *전에는*
// 어느 이미지인지 모른다. 반대로 `renderFinished`는 이미지가 정해진 뒤에 쓰므로
// 이미지당이고, 그래서 `SwapchainImage` 안에 있다.
// ---------------------------------------------------------------------------
//
// 프레임 상태(어느 이미지에 그리는 중인가)를 **멤버로 들지 않는다.** 진입점마다 뷰포트를
// 받아서 거기에 묻는다 - 이 객체는 매 프레임 재사용되므로, 프레임에만 유효한 값을 여기
// 두면 "지난 프레임 것"과 구별할 수 없다.
class VulkanCommandContext final : public RHICommandContext {
public:
    // device를 참조로 받는다: 펜스·세마포어 생성/파괴가 전부 디바이스 레벨 함수다 (기준 B).
    static std::unique_ptr<VulkanCommandContext> Create(const VulkanDevice& device) noexcept;

    ~VulkanCommandContext() override;

    // --- RHICommandContext: 상위가 부르는 것 ---
    void BeginRenderPass(RHIViewport& target) noexcept override;
    void EndRenderPass() noexcept override;

    // --- 여기부터는 RHI 모듈 안에서만 보인다 (D57) ---
    // VulkanRHI가 프레임을 여닫는 데 쓴다. 상위에는 이름조차 안 보인다.

    // 이전 프레임이 끝나기를 기다린다 (13단계의 1).
    void WaitForPreviousFrame() const noexcept;

    // acquire가 성공한 뒤에 리셋한다 (3). acquire가 실패하면 이 프레임은 제출되지 않고,
    // 그때 이미 리셋해버렸으면 다음 WaitForFences가 영원히 걸린다.
    void ResetFence() const noexcept;

    // 기록을 연다 (4~5).
    void BeginRecording() const noexcept;

    // present 가능한 레이아웃으로 바꾸고 기록을 닫는다 (10~11).
    //
    // **EndRenderPass()가 아니라 여기 있는 이유**: 이 배리어는 "렌더패스가 끝났다"가
    // 아니라 "이제 화면에 내보낸다"는 뜻이다. 언리얼도 이 전이를
    // `FVulkanViewport::Present()` 안에서 한다 - 렌더패스 종료가 아니라 present의 준비다.
    void EndRecordingForPresent(const VulkanViewport& target) noexcept;

    VkSemaphore ImageAvailable() const noexcept { return imageAvailable_; }
    VkFence InFlight() const noexcept { return inFlight_; }
    VkCommandBuffer Buffer() const noexcept;

private:
    VulkanCommandContext(const VulkanDevice& device,
                         VkSemaphore imageAvailable,
                         VkFence inFlight) noexcept;

    const VulkanDevice& device_;   // 파괴에 필요한 비소유 상태 (기준 A ②)

    // acquire가 신호하고 제출이 기다린다. "이미지를 써도 되는 시점"을 GPU에 알린다.
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;

    // 제출이 신호하고 다음 프레임의 CPU가 기다린다. 커맨드 버퍼를 다시 기록해도 되는
    // 시점을 정한다. **신호된 상태로 만든다** - 첫 프레임엔 기다릴 이전 프레임이 없다.
    VkFence inFlight_ = VK_NULL_HANDLE;
};

} // namespace LambdaEngine
