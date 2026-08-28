#pragma once

#include <volk.h>

#include <memory>

namespace LambdaEngine {

class VulkanDevice;

// 커맨드 풀 하나와 거기서 나온 커맨드 버퍼 하나. **네이티브 래퍼다.**
//
// **디바이스가 소유한다.** 커맨드 풀은 큐 패밀리에 묶이고(`queueFamilyIndex`) 창도
// 프레임도 모른다 - 디바이스가 사는 동안 살아 있으면서 매 프레임 리셋해 재사용되고,
// 디바이스와 함께 죽는다. 다만 **만드는 것은 디바이스가 아니다**(D96, 아래 Create 참고).
//
// ---------------------------------------------------------------------------
// **한때 이름이 `VulkanCommandContext`였는데 틀린 층의 이름이었다.**
//
// 언리얼에서 "CommandContext"는 `FVulkanCommandListContext : IRHICommandContext`,
// 즉 **렌더러가 대고 말하는 구현체**다. 그건 풀을 `FVulkanCommandBufferPool&`로
// **참조만** 하고(`VulkanContext.h:229`), 풀 자체는 `FVulkanCommandBufferPool`이라는
// 별도 타입이다. 우리도 같은 자리에 같은 이름을 둔다:
//
//   NativeWrappers/VulkanCommandPool      여기. VkCommandPool + VkCommandBuffer
//   Implementations/VulkanCommandContext  RHICommandContext 구현. 이것을 참조한다
// ---------------------------------------------------------------------------
//
// 풀과 버퍼를 한 클래스로 둔 것은 둘이 붙어 다니기 때문이다. 갈라야 할 이유가 생기면
// (스레드마다 풀, frames-in-flight마다 버퍼) 이 클래스 **안에서** 나눈다.
//
// 버퍼를 따로 반납하지 않는 것은 풀을 파괴하면 같이 사라지기 때문이다.
//
// frames-in-flight를 늘리면 이 묶음이 그 수만큼 필요해진다. 지금은 1이라 하나다 (D62).
class VulkanCommandPool {
public:
    // device를 참조로 받는다: 풀 생성·파괴가 전부 디바이스 레벨 함수다 (기준 B).
    // **디바이스가 만들어진 뒤에 디바이스가 부른다** - 그 전에는 큐 패밀리도 테이블도 없다.
    static std::unique_ptr<VulkanCommandPool> Create(const VulkanDevice& device) noexcept;

    ~VulkanCommandPool();

    VulkanCommandPool(const VulkanCommandPool&) = delete;
    VulkanCommandPool& operator=(const VulkanCommandPool&) = delete;
    VulkanCommandPool(VulkanCommandPool&&) = delete;
    VulkanCommandPool& operator=(VulkanCommandPool&&) = delete;

    // --- 커맨드 버퍼 하나만 놓고도 의미가 성립하는 연산 (기준 C) ---

    // 매 프레임 같은 버퍼를 다시 기록한다. 풀을 RESET_COMMAND_BUFFER로 만들어서
    // 버퍼 하나만 개별 리셋할 수 있다.
    void Reset() const noexcept;

    // ONE_TIME_SUBMIT: 한 번 제출하고 버릴 기록이라고 드라이버에 알린다.
    void Begin() const noexcept;
    void End() const noexcept;

    // 기록 대상. vkCmd~를 부르려면 이 핸들이 필요하다.
    VkCommandBuffer Buffer() const noexcept { return buffer_; }

private:
    VulkanCommandPool(const VulkanDevice& device,
                         VkCommandPool pool,
                         VkCommandBuffer buffer) noexcept;

    const VulkanDevice& device_;   // 파괴에 필요한 비소유 상태 (기준 A ②)

    VkCommandPool pool_ = VK_NULL_HANDLE;

    // 풀에서 나왔다. 우리가 파괴하지 않는다 - 풀과 함께 사라진다 (기준 A ③).
    VkCommandBuffer buffer_ = VK_NULL_HANDLE;
};

} // namespace LambdaEngine
