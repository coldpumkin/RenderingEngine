#pragma once

#include "Vulkan/Instance.h"
#include "Vulkan/Swapchain.h"

#include <memory>

struct GLFWwindow;

// ============================================================================
// 2. 창 시스템 (프로세스 단위) + 창 (창 단위)
// ============================================================================
//
// 둘을 나눈 이유: **glfwInit/glfwTerminate는 창이 아니라 프로세스 단위다.**
// 창이 둘이 돼도 초기화는 한 번이고, glfwTerminate는 **모든** 창을 부순다.
// glfwInit/glfwTerminate를 감싼다. **모든 창보다 오래 살아야 하므로 제일 먼저 선언한다.**
struct WindowSystem {
    bool initialized = false;

    WindowSystem() = default;
    ~WindowSystem();
    WindowSystem(const WindowSystem&) = delete;
    WindowSystem& operator=(const WindowSystem&) = delete;
};

bool InitWindowSystem(WindowSystem* out) noexcept;

// 스왑체인 자료구조 - **Window가 값으로 들기 때문에 여기서 먼저 정의한다.**
// 만들고 부수는 함수는 5절(디바이스가 생긴 뒤)에 흐름 순서대로 있다.
// ---------------------------------------------------------------------------

// 이미지 한 장에 딸린 것 전부. 인덱스 하나로 함께 지목되므로 한 몸으로 둔다
// (셋을 병렬 vector로 들면 개수가 어긋나도 컴파일된다).

// ============================================================================
// 창 하나에 묶인 것 전부
// ============================================================================
//
// **셋의 관계가 중첩이다:**
//
//   handle    ⊃  surface        ⊃  swapchain
//   (GLFW)       (인스턴스 레벨)     (디바이스 레벨)
//
// 파괴 순서가 그 중첩을 그대로 따른다. 그런데 **수명은 안쪽 하나만 다르다** -
// handle과 surface는 창이 사는 동안 그대로인데 swapchain만 리사이즈·최소화마다 갈린다.
//
// ---------------------------------------------------------------------------
// **서피스가 여기 있고 Swapchain 안에 없는 이유** (근거 셋)
//
//   1. 서피스는 **디바이스보다 먼저 존재한다.** PickPhysicalDevice가 "이 큐 패밀리가
//      이 창에 present 되나"를 물을 때 이미 필요하다. 그 시점엔 스왑체인은커녕
//      디바이스도 없다. 스왑체인의 부품이라면 있을 수 없는 일이다.
//   2. **레벨이 다르다.** 서피스는 인스턴스 레벨, 스왑체인은 디바이스 레벨이다.
//   3. **1:N이다.** 한 서피스에 스왑체인이 시간에 걸쳐 여럿 생긴다(동시엔 하나).
//
// 언리얼은 반대로 FVulkanSwapChain이 Surface를 멤버로 든다. 그랬더니 스왑체인이
// 죽을 때마다 서피스를 밖으로 빼내는 구조체가 따로 필요해졌다:
//   struct FVulkanSwapChainRecreateInfo { VkSwapchainKHR SwapChain; VkSurfaceKHR Surface; };
// **부품인데 부모보다 오래 살아야 한다는 건 부품이 아니라는 뜻이다.**
// 최종 파괴도 결국 FVulkanViewport(창을 대표하는 객체)가 한다.
// ---------------------------------------------------------------------------
//
// swapchain이 비어 있는 것은 **오류가 아니라 정상 상태다** (최소화 중). 그래서 이
// struct를 두 단계로 채우는 것(창+서피스 먼저, 스왑체인은 디바이스가 생긴 뒤)이
// "생성됐지만 못 쓰는 객체"를 만들지 않는다 - 그 상태가 원래 합법이다.
struct Window {
    GLFWwindow* handle = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    // **포맷은 스왑체인이 아니라 서피스의 성질이다.**
    //
    // vkGetPhysicalDeviceSurfaceFormatsKHR이 주는 목록은 (GPU, 서피스) 쌍으로 정해지고
    // 리사이즈로 바뀌지 않는다. 스왑체인은 그걸 **쓸 뿐**이다.
    //
    // 여기 있으면 파이프라인이 스왑체인을 기다릴 필요가 없다 - 한때 파이프라인 포맷
    // 하나를 얻으려고 루프 앞에서 스왑체인을 미리 만들었다.
    //
    // GPU가 정해져야 알 수 있으므로 OpenWindow가 아니라 SelectSurfaceFormat이 채운다.
    VkSurfaceFormatKHR surfaceFormat{};

    // **unique_ptr인 이유**: 리사이즈마다 통째로 갈아끼운다. 값으로 두면 이동 대입이
    // 필요하고, 포인터면 그게 공짜다. nullptr이 그대로 "지금 그릴 곳이 없다"를 뜻한다.
    std::unique_ptr<Swapchain> swapchain;

    // **창마다 하나여야 한다.** 한동안 전역이었는데, 그러면 창이 둘일 때 어느 창이
    // 바뀌었는지 구분할 수 없다. 창 하나뿐이라 안 터지고 있었을 뿐이다.
    bool swapchainOutOfDate = false;

    // 서피스 파괴가 인스턴스 레벨이라 인스턴스가 필요하다.
    // (스왑체인은 자기 dev를 들고 있으므로 여기선 dev가 필요 없다.)
    const VulkanInstance* inst = nullptr;

    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};

// 창 크기가 바뀌었다는 표시만 한다.
//
// 크기 값을 안 받는 이유: 실제 크기는 서피스에게 물어야 정확하고(DPI 스케일링),
// 여기 오는 값과 다를 수 있다. 그리고 콜백은 아무 때나 오므로 그 자리에서 재생성하면
// GPU가 프레임 중일 수 있다. **표시만 하고 다음 프레임 시작에 처리한다.**
//
// user pointer로 **어느 창인지** 찾는다. 이게 전역 플래그를 없앤 자리다.
// 창 + 서피스까지 만든다. 스왑체인은 디바이스가 생긴 뒤라 여기서 못 만든다.
//
// **out으로 받는 이유**: glfwSetWindowUserPointer에 넣을 주소가 **호출자가 들고 있을
// 최종 주소**여야 한다. 값으로 반환하면 함수 안의 임시 객체 주소를 넣게 된다.
bool OpenWindow(const VulkanInstance& inst,
                int width, int height, const char* title,
                Window* out) noexcept;

// 지금 그릴 수 있는 크기인가. **최소화하면 프레임버퍼가 0x0이 된다.**
//
// **왜 루프 맨 위에서 이걸 묻나**: 안 물으면 최소화 중에 EnsureSwapchain이 매 순회마다
// vkDeviceWaitIdle + 서피스 조회 + 스왑체인 생성을 재시도한다. present를 안 하니
// 수직동기 제동도 없다. 실측으로 **CPU 10.9% -> 135.9%** (12배)였다.
//
// 이 질문은 "프레임을 아예 돌릴 것인가"이고, BeginFrame의 "프레임이 어떻게 진행되는가"와
// 다르다. 그래서 BeginFrame이 아니라 루프에 있다.
bool WindowHasDrawableSize(const Window& window) noexcept;

// 이 서피스가 받는 포맷 중 하나를 골라 window->surfaceFormat에 담는다.
// **GPU가 정해진 뒤에 부른다** - 어떤 포맷을 받는지는 (GPU, 서피스) 쌍이 정한다.
// 실패하면 false (서피스가 포맷을 하나도 안 준 경우).
bool SelectSurfaceFormat(const VulkanInstance& inst,
                         const struct VulkanDevice& dev,
                         Window* window) noexcept;