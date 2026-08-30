#pragma once

#include "Vulkan/Core.h"

// Instance
// ============================================================================
//
// Table과 handle을 한 묶음에 두는 이유: 호출이 두 table 중 하나로 갈리므로
// (실측: instance 26곳 / device 30곳) handle과 table이 한 곳에서 나와야 섞일 수 없다.
//
// 필요 범위는 서로 다르다 - table은 거의 모든 곳에, handle은 만들고 부수는 곳에만
// 필요하다. 물리 device 조회는 gpu로 dispatch하지 instance로 하지 않기 때문이다.
struct VulkanInstance {
    VolkInstanceTable table{};
    VkInstance handle = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    VulkanInstance() = default;
    ~VulkanInstance();
    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;
};

// loader 확인 -> instance -> function table -> debug messenger.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
bool CreateInstance(VulkanInstance* out) noexcept;
