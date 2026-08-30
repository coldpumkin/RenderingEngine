#pragma once

#include "Config.h"
#include "Vulkan/Device.h"

// Command pool - queue family마다 하나
// ============================================================================
//
// 스펙 제약: pool은 queueFamilyIndex로 만들어지고, 그 pool에서 나온 command buffer는
// 같은 family의 queue에만 제출할 수 있다.
//
// Pool과 buffer는 수명이 다르다. Pool은 device가 사는 동안 그대로고, buffer는 매
// frame 리셋해 다시 기록한다(GPU가 다 쓴 뒤에만).
//
// Pool 개수는 세 축의 곱이다: queue family(3) x thread(1) x frames-in-flight(1).
//   thread 축           pool은 thread-safe가 아니다
//   frames-in-flight 축 지금은 buffer를 개별 리셋한다(RESET_COMMAND_BUFFER).
//                       vkResetCommandPool로 바꾸면 frame마다 pool이 필요해진다.
//
// 현재 정책: compute/transfer pool은 만들어만 두고 아무것도 제출하지 않는다.
// 전용 family가 없으면 VK_NULL_HANDLE이고 그 일은 graphics가 한다.
struct Commands {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    VkCommandPool graphics = VK_NULL_HANDLE;
    VkCommandPool compute  = VK_NULL_HANDLE;
    VkCommandPool transfer = VK_NULL_HANDLE;

    Commands() = default;
    ~Commands();
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;
};

bool CreateCommands(const VulkanDevice& dev, Commands* out) noexcept;
