#pragma once

#include "Vulkan/Texture.h"

#include <memory>
#include <vector>

// Swapchain - 여기만 수명이 있다
// ============================================================================
//
// 다른 것들은 한 번 만들고 끝까지 가는데 swapchain만 창 크기가 바뀔 때마다 다시
// 만든다. "같이 바뀌는가" 축에서 유일하게 혼자 움직이는 덩어리다.

// 앞 선언: EnsureSwapchain이 창을 받는다. Window가 Swapchain을 품으므로
// Window.h가 이 파일을 include하고, 여기서는 역방향으로 이름만 안다.
struct Window;

// 그려지는 image이므로 Texture다 - 우리 attachment와 같은 타입이고, 그래서 무슨
// format인지 desc가 말한다. 다른 점은 allocation이 없다는 것뿐이다: image는 조회한
// 것이고 view만 우리가 만든다.
//
// swapchain이 소유한 자원이고 밖으로 나가지 않는다. 밖으로 나가는 것은 이 중 하나를
// 이번 프레임 목적지로 지목한 FrameTarget이고, index도 거기 있다 - 배열 위치를 여기
// 복사해두면 그 사본이 낡을 수 있다.
struct SwapchainImage {
    Texture texture;
    VkSemaphore renderFinished = VK_NULL_HANDLE;  // image당 하나 (이유는 .cpp에)
};

struct Swapchain {
    const struct VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<SwapchainImage> images;

    Swapchain() = default;
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
};

// 실패 또는 "지금은 만들 수 없음"(최소화)이면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
bool CreateSwapchain(const VulkanInstance& inst,
                     const VulkanDevice& dev,
                     VkSurfaceKHR surface,
                     VkSurfaceFormatKHR surfaceFormat,
                     VkSwapchainKHR oldSwapchain,
                     Swapchain* out) noexcept;

// Effect: window->surfaceFormat을 채운다
//
// Logical device가 아니라 physical device를 받는다 - GPU에게 묻는 조회라
// vkCreateDevice 전에도 부를 수 있다. 한때 VulkanDevice 전체를 받으면서 .gpu만 썼는데,
// 그러면 "device가 있어야 한다"고 시그니처가 거짓말을 한다.
bool SelectSurfaceFormat(const VulkanInstance& inst,
                         VkPhysicalDevice gpu,
                         Window* window) noexcept;

// Effect: 낡았거나 없으면 window->swapchain을 다시 만든다
// Output: false는 "지금 그릴 곳이 없다"이고, 그것이 실패인지는 **호출자가 정한다**.
//         main의 초기화에서는 켤 수 없다는 뜻이라 치명적이고, BeginFrame에서는
//         최소화 중이라는 뜻이라 그 프레임만 건너뛴다.
//
// Instance를 안 받는다 - window가 자기를 만든 instance를 들고 있다. 밖에서 또 받으면
// 다른 instance를 넘길 수 있는 구멍이 생기고, 그건 컴파일러가 못 잡는다.
bool EnsureSwapchain(const VulkanDevice& dev, Window* window) noexcept;

// 교체된 swapchain. 아직 파괴하면 안 된다 - present가 그 image를 읽고 있을 수 있는데,
// **present에는 완료 신호가 없다** (vkQueuePresentKHR은 fence를 주지 않는다).
//
// 그래서 기다릴 대상이 없고 셀 수 있는 것만 있다: 새 swapchain에서 image 수만큼
// present가 더 일어나면 옛 image를 표시하던 것은 전부 교체됐다. **이것은 증명이 아니라
// 셈이다** - 정확한 답은 VK_KHR_swapchain_maintenance1의 present fence이고, 그 확장이
// 따로 생겼다는 것 자체가 원래 답이 없었다는 증거다.
//
// 셈이 틀리면 조용하지 않다: 사용 중인 swapchain 파괴는 VUID 위반이라 검증 레이어가
// 잡는다. 그것이 지금 이 값을 검증하는 유일한 수단이다.
struct RetiredSwapchain {
    std::unique_ptr<Swapchain> swapchain;
    uint32_t framesLeft = 0;
};

// Effect: 셈이 끝난 retired swapchain을 파괴한다. 프레임마다 한 번 불린다.
//
// 여기가 "자원이 소멸자 순서에서 풀려나는" 첫 자리다. 소유자가 놓는 시점과 실제 파괴
// 시점이 갈렸고, 그 사이를 프레임 수가 잇는다.
void AdvanceRetiredSwapchains(Window* window) noexcept;
