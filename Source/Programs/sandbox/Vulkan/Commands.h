#pragma once

#include "Config.h"
#include "Vulkan/Device.h"

// 6. 커맨드 풀 (큐 패밀리마다) + 프레임 자원 (frames-in-flight마다)
// ============================================================================
//
// **둘은 수명이 다르다.** 한 덩어리로 두면 개수를 늘릴 때 같이 늘어나 버린다.
//
//   풀    큐 패밀리에 묶인다. 디바이스가 사는 동안 그대로
//   버퍼  매 프레임 리셋해 다시 기록한다. **GPU가 다 쓴 뒤에만** 리셋할 수 있다
//
// 언리얼도 같다: FVulkanCommandBufferPool은 **큐당** 하나이고
// 그 안에서 TArray<FVulkanCommandBuffer*>를 재활용한다.

// ---------------------------------------------------------------------------
// 커맨드 풀 - 큐 패밀리마다 하나
// ---------------------------------------------------------------------------
//
// 스펙 제약이라 선택의 여지가 없다: 풀은 queueFamilyIndex로 만들어지고,
// **그 풀에서 나온 커맨드 버퍼는 같은 패밀리의 큐에만 제출할 수 있다.**
//
// 풀의 개수는 세 축의 곱이다:
//   큐 패밀리(3) x 스레드(1) x frames-in-flight(1) = 3
//
// 스레드 축: 풀은 **스레드 안전이 아니다**(외부 동기화 필요).
// frames-in-flight 축: 지금은 버퍼를 개별 리셋하므로(RESET_COMMAND_BUFFER) 풀은 하나면
// 된다. 프레임 단위로 vkResetCommandPool을 쓰게 되면 그때 프레임마다 풀이 필요해진다.
//
// **compute/transfer 풀은 아직 아무것도 제출하지 않는다.** 만들어만 두고,
// 실제 작업(업로드·디스패치)이 생길 때 그 패밀리의 버퍼를 뽑는다.
// 전용 패밀리가 없으면 VK_NULL_HANDLE이고, 그 일은 graphics가 한다.
struct Commands {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 비소유 상태

    VkCommandPool graphics = VK_NULL_HANDLE;
    VkCommandPool compute  = VK_NULL_HANDLE;
    VkCommandPool transfer = VK_NULL_HANDLE;
    Commands() = default;
    ~Commands();
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;
};

bool CreateCommands(const VulkanDevice& dev, Commands* out) noexcept;

// 개수는 Config.h의 kFramesInFlight가 정한다.
struct Frame {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 비소유 상태

    // graphics 풀에서 나온다. 풀이 죽으면 같이 사라지므로 따로 반납하지 않는다.
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    Frame() = default;
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

bool CreateFrame(const VulkanDevice& dev, const Commands& commands, Frame* out) noexcept;