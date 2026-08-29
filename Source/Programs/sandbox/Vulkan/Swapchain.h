#pragma once

#include "Vulkan/Device.h"

#include <vector>

// 앞 선언: EnsureSwapchain이 창을 받는다. Window가 Swapchain을 품으므로
// Window.h가 이 파일을 include하고, 여기서는 역방향으로 이름만 안다.
struct Window;

struct SwapchainImage {
    VkImage image = VK_NULL_HANDLE;               // 스왑체인이 소유. 우리가 파괴하지 않는다
    VkImageView view = VK_NULL_HANDLE;            // 우리가 만들었다 -> 우리가 파괴한다
    VkSemaphore renderFinished = VK_NULL_HANDLE;  // 우리가 만들었다. **이미지당 하나**
};

struct Swapchain {
    // 파괴에 필요한 비소유 상태 - 소멸자는 인자를 못 받는다.
    const struct VulkanDevice* dev = nullptr;

    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<SwapchainImage> images;

    Swapchain() = default;
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
};

// 5. 스왑체인 - **여기만 수명이 있다**
// ============================================================================
//
// 다른 것들은 전부 한 번 만들고 끝까지 간다. 스왑체인만 창 크기가 바뀔 때마다
// 다시 만든다. **"같이 바뀌는가" 축에서 유일하게 혼자 움직이는 덩어리**라
// 클래스로 뺄 근거가 이미 가장 뚜렷하다.

// SRGB를 우선하는 이유: 모니터는 선형이 아니라 감마 곡선으로 빛을 낸다. 포맷에 _SRGB가
// 붙어 있으면 GPU가 그 변환을 하드웨어로 해준다. UNORM을 쓰면 셰이더에서 직접 감마
// 보정을 해야 하고, 안 하면 화면이 어둡게 나온다.
// 실패 또는 "지금은 만들 수 없음"(최소화)이면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
bool CreateSwapchain(const VulkanInstance& inst,
                     const VulkanDevice& dev,
                     VkSurfaceKHR surface,
                     VkSurfaceFormatKHR surfaceFormat,
                     VkSwapchainKHR oldSwapchain,
                     Swapchain* out) noexcept;

// 그릴 곳을 보장한다. 낡았거나 없으면 다시 만든다.
// **false는 실패가 아니라 "지금은 그릴 곳이 없다"** (최소화 중)이다.
//
// **인스턴스를 인자로 받지 않는다.** window가 이미 자기를 만든 인스턴스를 들고 있다.
// 밖에서 또 받으면 *다른* 인스턴스를 넘길 수 있는 구멍이 생기는데, 그건 컴파일러가
// 못 잡는다. 아예 안 받으면 그 실수가 불가능해진다.
bool EnsureSwapchain(const VulkanDevice& dev, Window* window) noexcept;