#pragma once

#include "Vulkan/Attachments.h"
#include "Vulkan/Instance.h"
#include "Vulkan/Swapchain.h"

#include <memory>

struct GLFWwindow;

// Window system (프로세스 단위) + Window (창 단위)
// ============================================================================
//
// 둘을 나눈 이유: glfwInit/glfwTerminate는 창이 아니라 프로세스 단위다. 창이 둘이 돼도
// 초기화는 한 번이고 glfwTerminate는 모든 창을 부순다.
//
// 모든 창보다 오래 살아야 하므로 제일 먼저 선언한다.
struct WindowSystem {
    bool initialized = false;

    WindowSystem() = default;
    ~WindowSystem();
    WindowSystem(const WindowSystem&) = delete;
    WindowSystem& operator=(const WindowSystem&) = delete;
};

bool InitWindowSystem(WindowSystem* out) noexcept;

// 창 하나에 묶인 것 전부
// ============================================================================
//
// 셋의 관계가 중첩이다:
//   handle(GLFW)  >  surface(instance level)  >  swapchain(device level)
//
// 파괴 순서가 그 중첩을 따른다. 수명은 안쪽 하나만 다르다 - handle과 surface는 창이
// 사는 동안 그대로고 swapchain만 리사이즈·최소화마다 갈린다.
//
// Surface가 Swapchain 안이 아니라 여기 있는 근거 셋:
//   1. Surface는 device보다 먼저 존재한다. PickPhysicalDevice가 "이 queue family가
//      이 창에 present 되나"를 물을 때 이미 필요하다
//   2. Level이 다르다 - surface는 instance, swapchain은 device
//   3. 1:N이다 - 한 surface에 swapchain이 시간에 걸쳐 여럿 생긴다
//
// Unreal은 반대로 FVulkanSwapChain이 Surface를 멤버로 든다. 그랬더니 swapchain이 죽을
// 때마다 surface를 밖으로 빼내는 구조체(FVulkanSwapChainRecreateInfo)가 따로 필요해졌다.
//
// swapchain이 비어 있는 것은 오류가 아니라 정상 상태다(최소화 중).
struct Window {
    GLFWwindow* handle = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    // Format은 swapchain이 아니라 surface의 성질이다 - (GPU, surface) 쌍으로 정해진다.
    // 여기 있으면 pipeline이 swapchain을 기다릴 필요가 없다. GPU가 정해져야 알 수 있어서
    // OpenWindow가 아니라 SelectSurfaceFormat이 채운다.
    //
    // **"리사이즈로 바뀌지 않는다"고 적혀 있었는데, 그건 스펙이 보장하는 것보다 센
    // 주장이었다.** vkGetPhysicalDeviceSurfaceFormatsKHR의 결과는 고정이 아니다 -
    // 창이 다른 모니터로 가거나 HDR이 켜지면 달라질 수 있다. 리사이즈만 놓고 보면
    // 맞는 말이지만 그 셋을 다 덮지는 못한다.
    //
    // 그래서 EnsureSwapchain이 재생성할 때마다 다시 묻고, 바뀌면 아래 플래그를 세운다.
    VkSurfaceFormatKHR surfaceFormat{};

    // unique_ptr인 이유: 리사이즈마다 통째로 갈아끼운다. 값으로 두면 move 대입이
    // 필요하고, 포인터면 그게 공짜다. nullptr이 그대로 "지금 그릴 곳이 없다"를 뜻한다.
    std::unique_ptr<Swapchain> swapchain;

    // 창마다 하나여야 한다. 한동안 전역이었는데, 그러면 창이 둘일 때 어느 창이
    // 바뀌었는지 구분할 수 없다.
    bool swapchainOutOfDate = false;


    // Surface 파괴가 instance level이라 instance가 필요하다.
    const VulkanInstance* inst = nullptr;

    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};

// 창 + surface까지 만든다. Swapchain은 device가 생긴 뒤라 여기서 못 만든다.
//
// out으로 받는 이유: glfwSetWindowUserPointer에 넣을 주소가 호출자가 들고 있을 최종
// 주소여야 한다. 값으로 반환하면 함수 안의 임시 객체 주소를 넣게 된다.
bool OpenWindow(const VulkanInstance& inst,
                int width, int height, const char* title,
                Window* out) noexcept;

// Output: 지금 그릴 수 있는 크기인가. 최소화하면 framebuffer가 0x0이 된다.
//
// 루프 맨 위에서 이걸 묻는 이유: 안 물으면 최소화 중에 EnsureSwapchain이 매 순회마다
// vkDeviceWaitIdle + surface 조회 + swapchain 생성을 재시도하는데, present가 없어
// vsync 제동도 없다. 실측 CPU 10.9% -> 135.9%.
//
// "frame을 아예 돌릴 것인가"는 창의 상태이지 BeginFrame이 답할 질문이 아니다.
bool WindowHasDrawableSize(const Window& window) noexcept;

// Effect: window->surfaceFormat을 채운다
//
// Logical device가 아니라 physical device를 받는다 - GPU에게 묻는 조회라
// vkCreateDevice 전에도 부를 수 있다. 한때 VulkanDevice 전체를 받으면서 .gpu만 썼는데,
// 그러면 "device가 있어야 한다"고 시그니처가 거짓말을 한다.
// Output: what the post-process pass draws into. The swapchain decides it, unlike our own
//         attachments; colorSpace stays behind because only swapchain creation reads it.
inline AttachmentFormats WindowAttachment(const Window& window) noexcept {
    return AttachmentFormats{window.surfaceFormat.format};
}

bool SelectSurfaceFormat(const VulkanInstance& inst,
                         VkPhysicalDevice gpu,
                         Window* window) noexcept;
