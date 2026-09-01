#pragma once

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

// 초기화 전용 one-shot command buffer
// ============================================================================
//
// Begin이 buffer 하나를 뽑아 기록을 열고, End가 닫고 제출하고 GPU를 기다린 뒤
// 반납한다. 사이에 vkCmd*를 적는다.
//
// **매 frame 경로에는 쓰지 않는다.** 안에 vkQueueWaitIdle이 있다 - 초기화라 기다려도
// 되는 것이지 싼 것이 아니다.
//
// graphics queue를 쓴다. transfer queue의 값어치는 그리는 동안 동시에 올리는 것인데
// 이건 루프 전에 한 번뿐이라 겹칠 대상이 없고, queue를 바꾸면 ownership transfer
// 비용만 낸다. 한 번 옮겨봤다가 되돌렸다(b104a27) - 필요해지면 그 commit을 꺼낸다.
//
// buffer upload와 texture upload 둘이 같은 절차를 쓰게 되면서 갈라져 나왔다.

// Output: 기록이 열린 command buffer (실패하면 VK_NULL_HANDLE)
VkCommandBuffer BeginOneShot(const VulkanDevice& dev, const Commands& commands) noexcept;

// Input:  what - 실패 log에 찍을 이름
// Effect: 성공/실패 어느 쪽이든 command buffer를 반납한다
bool EndOneShotAndWait(const VulkanDevice& dev, const Commands& commands,
                       VkCommandBuffer cmd, const char* what) noexcept;
