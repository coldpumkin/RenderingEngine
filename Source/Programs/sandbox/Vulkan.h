#pragma once

// 이 프로그램이 다루는 것들 - **어휘집**.
//
// 구조는 셋으로 나뉜다:
//
//   Vulkan.h         무엇이 있는가        (struct 정의 + 함수 선언)
//   VulkanSetup.cpp  어떻게 만들고 부수나  (초기화 경로. 한 번 돈다)
//   main.cpp         한 프레임이 어떻게 도나 (런타임 경로. 초당 수백 번 돈다)
//
// **파일을 나눈 기준이 "초기화 vs 런타임"이다.** 둘은 지켜야 할 규칙이 다르다 -
// 초기화는 힙 할당도 로그도 자유롭지만, 런타임은 매 프레임 돌아서 둘 다 조심해야 한다.
//
// **클래스가 없다. 의도적이다.** 여기 있는 것은 전부 평범한 struct와 자유 함수다.
// 파일을 나눈 것은 흐름을 보기 위해서지 추상화를 넣기 위해서가 아니다.
// (RAII는 파괴 순서가 실제로 아플 때 넣는다. 그때 struct 모양은 안 바뀌고
//  소멸자만 붙는다.)

#include <volk.h>

#include <cstdio>
#include <vector>

struct GLFWwindow;

#define LOG(...)  std::fprintf(stderr, __VA_ARGS__)

// ============================================================================
// 이 엔진이 요구하는 것
// ============================================================================

// 다이나믹 렌더링이 1.3 코어라서 1.3이다. 임의로 고른 값이 아니다.
constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;

// 화면에 그리려면 반드시 있어야 하는 **디바이스** 확장.
// (VK_KHR_surface는 인스턴스 확장이다. 층이 다르다.)
constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// 버전을 지원해도 기능이 꺼져 있을 수 있어서 따로 확인한다.
// **확인할 때와 켤 때 같은 값을 봐야 한다** - 어긋나면 디바이스는 만들어지고 드로우에서 죽는다.
// inline: 헤더에 정의가 있으므로 여러 .cpp에 들어가도 중복 정의가 안 된다.
inline VkPhysicalDeviceVulkan13Features RequiredFeatures13() noexcept {
    VkPhysicalDeviceVulkan13Features features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.dynamicRendering = VK_TRUE;   // VkRenderPass/VkFramebuffer 없이 그린다
    features.synchronization2 = VK_TRUE;   // 배리어/제출 API 개정판
    return features;
}

// ============================================================================

// ============================================================================
// 1. 인스턴스
// ============================================================================
// 인스턴스 층. 셋이 같이 태어나고 같이 죽는다.
//
// **table과 handle을 한 묶음에 두는 이유**: Vulkan 호출은 예외 없이 두 테이블 중
// 하나로 갈린다(실측: 인스턴스 26곳 / 디바이스 30곳). 우리가 정한 선이 아니라
// API 자체의 선이고, 핸들과 테이블이 한 곳에서 나와야 섞일 수 없다.
//
// 다만 **필요 범위는 서로 다르다.** 테이블은 거의 모든 곳에 필요하지만 핸들은
// 만들고 부수는 곳에만 필요하다 - 물리 디바이스 조회는 gpu로 디스패치하지
// 인스턴스로 하지 않기 때문이다. 함께 넘기는 비용은 참조 하나라 묶는 쪽이 낫다.
//
// **아직 소멸자가 없다.** 정리는 main() 끝에 모여 있고, 그 순서가 실제로 아플 때
// RAII로 옮긴다. 그때 이 struct의 모양은 안 바뀐다 - 소멸자만 붙는다.
struct VulkanInstance {
    VolkInstanceTable table{};
    VkInstance handle = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
};

// 로더 확인 -> 인스턴스 -> 함수 테이블 -> 디버그 메신저.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// **out 파라미터가 없어졌다.** 셋이 한 묶음이 되니 반환값 하나면 된다 -
// out 파라미터는 애초에 "이것들은 같이 나온다"는 신호였다.
// 로더 확인 -> 인스턴스 -> 함수 테이블 -> 디버그 메신저.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
VulkanInstance CreateInstance() noexcept;

// ============================================================================
// 2. 창 시스템 (프로세스 단위) + 창 (창 단위)
// ============================================================================
//
// 둘을 나눈 이유: **glfwInit/glfwTerminate는 창이 아니라 프로세스 단위다.**
// 창이 둘이 돼도 초기화는 한 번이고, glfwTerminate는 **모든** 창을 부순다.
bool InitWindowSystem() noexcept;
void ShutdownWindowSystem() noexcept;

// 스왑체인 자료구조 - **Window가 값으로 들기 때문에 여기서 먼저 정의한다.**
// 만들고 부수는 함수는 5절(디바이스가 생긴 뒤)에 흐름 순서대로 있다.
// ---------------------------------------------------------------------------

// 이미지 한 장에 딸린 것 전부. 인덱스 하나로 함께 지목되므로 한 몸으로 둔다
// (셋을 병렬 vector로 들면 개수가 어긋나도 컴파일된다).
struct SwapchainImage {
    VkImage image = VK_NULL_HANDLE;               // 스왑체인이 소유. 우리가 파괴하지 않는다
    VkImageView view = VK_NULL_HANDLE;            // 우리가 만들었다 -> 우리가 파괴한다
    VkSemaphore renderFinished = VK_NULL_HANDLE;  // 우리가 만들었다. **이미지당 하나**
};

struct Swapchain {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<SwapchainImage> images;
};

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

    Swapchain swapchain;

    // **창마다 하나여야 한다.** 한동안 전역이었는데, 그러면 창이 둘일 때 어느 창이
    // 바뀌었는지 구분할 수 없다. 창 하나뿐이라 안 터지고 있었을 뿐이다.
    bool swapchainOutOfDate = false;
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

// 중첩의 역순으로 부순다: 스왑체인 -> 서피스 -> 창.
void CloseWindow(const VulkanInstance& inst, const struct VulkanDevice& dev,
                 Window* window) noexcept;

// 이 서피스가 받는 포맷 중 하나를 골라 window->surfaceFormat에 담는다.
// **GPU가 정해진 뒤에 부른다** - 어떤 포맷을 받는지는 (GPU, 서피스) 쌍이 정한다.
// 실패하면 false (서피스가 포맷을 하나도 안 준 경우).
bool SelectSurfaceFormat(const VulkanInstance& inst,
                         const struct VulkanDevice& dev,
                         Window* window) noexcept;

// 3. 물리 디바이스 고르기 + 큐 패밀리 고르기
// ============================================================================
//
// **큐 패밀리가 뭔가**
//
// GPU는 명령을 "큐"에 넣어야 실행한다. 그런데 모든 큐가 모든 일을 하지는 않는다.
// GPU는 큐를 **패밀리**로 묶어서 "이 그룹은 그래픽스+컴퓨트+전송을 다 하고, 저 그룹은
// 전송만 전담한다"는 식으로 알려준다. 전송 전담 패밀리는 보통 별도 DMA 엔진이라
// 그래픽스와 **물리적으로 병렬로** 돈다 - 그게 패밀리를 나눠 놓은 이유다.
//
// 스펙상 GRAPHICS나 COMPUTE 비트가 있으면 전송은 **암묵적으로 지원된다**
// (TRANSFER 비트가 안 켜져 있어도 된다). 그래서 "전송 가능한가"를 물으려고
// TRANSFER 비트를 보면 안 되고, "**전송만** 하는 전용 패밀리인가"를 물을 때 본다.
//
// 데스크톱 GPU의 전형적인 모습:
//   family 0 : GRAPHICS | COMPUTE | TRANSFER   범용 큐
//   family 1 : COMPUTE  | TRANSFER             async compute
//   family 2 : TRANSFER                        DMA 엔진
struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;   // **필수.** present도 여기서 한다
    uint32_t compute  = UINT32_MAX;   // 없을 수 있다
    uint32_t transfer = UINT32_MAX;   // 없을 수 있다

    bool HasCompute()  const noexcept { return compute  != UINT32_MAX; }
    bool HasTransfer() const noexcept { return transfer != UINT32_MAX; }
};

// **전용이 아니면 안 만든다.**
//
// 컴퓨트/전송 큐를 따로 두는 목적은 그래픽스와 **동시에** 도는 것이다. 같은 패밀리로
// 대체하면 그 이득은 없으면서, 큐가 갈리는 순간 생기는 비용은 그대로 낸다:
// 큐 사이 동기화(세마포어)와 **큐 패밀리 소유권 이전**(release/acquire 배리어 한 쌍).
//
// 그래서 전용 패밀리가 없으면 UINT32_MAX로 두고, 그 일은 그래픽스 큐가 한다.
// 언리얼도 같다 - 전용을 못 찾으면 Queues[AsyncCompute]를 nullptr로 둔다
// (VulkanDevice.cpp: "If we didn't find a dedicated Queue, leave it null").
//
// 그래픽스 하나만 못 찾으면 실패다. 화면에 못 그리면 이 엔진은 할 일이 없다.
struct PhysicalDeviceSelection {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    QueueFamilies families;
};

// GPU 고르기. 자격을 통과한 것 중 외장을 선호한다.
// 실패하면 gpu가 VK_NULL_HANDLE인 채로 돌아온다.
// GPU 고르기. 자격을 통과한 것 중 외장을 선호한다.
// 실패하면 gpu가 VK_NULL_HANDLE인 채로 돌아온다.
PhysicalDeviceSelection PickPhysicalDevice(const VulkanInstance& inst,
                                           VkSurfaceKHR surface) noexcept;

// 4. 논리 디바이스 + 함수 테이블 + 큐들
// ============================================================================

// 만든 큐 핸들들. **compute/transfer는 VK_NULL_HANDLE일 수 있다** -
// 전용 패밀리가 없다는 뜻이고, 그때 그 일은 graphics가 한다.
struct Queues {
    VkQueue graphics = VK_NULL_HANDLE;
    VkQueue compute  = VK_NULL_HANDLE;
    VkQueue transfer = VK_NULL_HANDLE;

    // **present는 4번째 큐가 아니라 역할이다.** 위 셋 중 하나를 가리키는 별칭이고,
    // 기본은 그래픽스다. 언리얼도 같다:
    //   FVulkanQueue* PresentQueue = nullptr;  // points to an existing queue
    //   (VulkanDevice.h:718, EVulkanQueueType은 Graphics/AsyncCompute/Transfer 셋뿐)
    //
    // 그래픽스가 present를 못 하는 하드웨어는 지원하지 않는다 - 언리얼도 그 경우
    // 메시지박스를 띄우고 종료한다(VulkanSwapChain.cpp:886 SetupPresentQueue).
    // 지원하려면 스왑체인을 CONCURRENT로 바꾸거나 소유권 이전을 넣어야 하고,
    // Win32 단일 GPU에서는 일어나지 않는 경우다.
    //
    // **나중에 볼 것**: AMD에서는 컴퓨트 큐로 present하는 빠른 경로가 있다.
    // 언리얼이 vendor를 AMD로 한정해 cvar 뒤에 두고 있다:
    //   bPresentOnComputeQueue = (VendorId == EGpuVendorId::Amd);
    // 제출 구조가 바뀌므로 **지연을 실제로 잴 수 있을 때** 검토한다.
    VkQueue present = VK_NULL_HANDLE;

    // 없으면 그래픽스로 떨어진다. 호출부가 매번 분기하지 않게.
    VkQueue ComputeOrGraphics()  const noexcept { return compute  ? compute  : graphics; }
    VkQueue TransferOrGraphics() const noexcept { return transfer ? transfer : graphics; }
};

// 디바이스 층. **다섯이 한 몸이다.**
//
// 근거: 디바이스가 생긴 뒤로 gpu · families · queues가 device 없이 쓰이는 곳이
// 하나도 없다. 반대로 device를 쓰는 곳은 거의 다 table도 같이 쓴다.
// 그래서 PhysicalDeviceSelection이 여기로 흡수된다 - 선택 결과는 디바이스의 정체다.
//
// 인스턴스와 같은 이유로 table과 handle이 같이 있고, 같은 이유로 필요 범위는 다르다:
// vkCmd*는 커맨드 버퍼로 디스패치하므로 기록 함수는 table만 있으면 되고 handle은 필요 없다.
//
// **아직 소멸자가 없다.** 인스턴스와 같다 - 정리 순서가 아플 때 RAII로 옮긴다.
struct VulkanDevice {
    VolkDeviceTable table{};
    VkDevice handle = VK_NULL_HANDLE;

    // 선택 결과가 여기로 흡수됐다. 파괴할 것이 없는 값들이라 소유가 아니다.
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    QueueFamilies families;

    // vkGetDeviceQueue는 **조회**다. vkCreateDevice가 이미 만들었고 파괴 함수도 없다.
    Queues queues;

    // 이 GPU의 메모리 타입 목록. **여기 두는 이유**(기준 A ③):
    //   생성 시 확정 · 사는 동안 불변 · 디바이스가 죽으면 의미 상실 - 셋 다 참이다.
    //
    // 조회 함수(vkGetPhysicalDeviceMemoryProperties)는 **인스턴스 레벨**이라 나중에
    // 다시 물으려면 인스턴스가 필요하다. 버퍼를 만들 때마다 인스턴스를 끌고 다니는
    // 대신, 안 변하는 값이니 여기 한 번 담아둔다.
    VkPhysicalDeviceMemoryProperties memoryProperties{};
};

// 논리 디바이스 + 함수 테이블 + 큐들.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// **inst를 받는 이유**: vkCreateDevice는 **인스턴스 레벨 함수**다. 만드는 함수와
// 파괴하는 함수(vkDestroyDevice, 디바이스 레벨)의 층이 다르다는 Vulkan API의 비대칭이고,
// 그래서 "이 클래스가 무슨 레벨이냐"가 아니라 "이 호출이 무슨 레벨이냐"로 봐야 한다.
// 논리 디바이스 + 함수 테이블 + 큐들.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// **inst를 받는 이유**: vkCreateDevice는 **인스턴스 레벨 함수**다. 만드는 함수와
// 파괴하는 함수(vkDestroyDevice, 디바이스 레벨)의 층이 다르다는 Vulkan API의 비대칭이고,
// 그래서 "이 클래스가 무슨 레벨이냐"가 아니라 "이 호출이 무슨 레벨이냐"로 봐야 한다.
VulkanDevice CreateDevice(const VulkanInstance& inst,
                          const PhysicalDeviceSelection& selection) noexcept;

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
Swapchain CreateSwapchain(const VulkanInstance& inst,
                          const VulkanDevice& dev,
                          VkSurfaceKHR surface,
                          VkSurfaceFormatKHR surfaceFormat,
                          VkSwapchainKHR oldSwapchain) noexcept;

void DestroySwapchain(const VulkanDevice& dev, Swapchain* sc) noexcept;

// 그릴 곳을 보장한다. 낡았거나 없으면 다시 만든다.
// **false는 실패가 아니라 "지금은 그릴 곳이 없다"** (최소화 중)이다.
bool EnsureSwapchain(const VulkanInstance& inst,
                     const VulkanDevice& dev,
                     Window* window) noexcept;

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
    VkCommandPool graphics = VK_NULL_HANDLE;
    VkCommandPool compute  = VK_NULL_HANDLE;
    VkCommandPool transfer = VK_NULL_HANDLE;
};

bool CreateCommands(const VulkanDevice& dev, Commands* out) noexcept;
void DestroyCommands(const VulkanDevice& dev, Commands* c) noexcept;

struct Frame {
    // graphics 풀에서 나온다. 풀이 죽으면 같이 사라지므로 따로 반납하지 않는다.
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
};

bool CreateFrame(const VulkanDevice& dev, const Commands& commands, Frame* out) noexcept;
void DestroyFrame(const VulkanDevice& dev, Frame* frame) noexcept;

// ============================================================================
// 7. 그래픽스 파이프라인 - **무엇으로 그리는가**
// ============================================================================
//
// **파이프라인은 스왑체인 포맷에 묶인다** (VkPipelineRenderingCreateInfo의
// pColorAttachmentFormats). 크기에는 안 묶인다 - 뷰포트/시저를 동적 상태로 뒀다.
// 그래서 리사이즈로는 재생성이 필요 없다.
struct Pipeline {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline handle = VK_NULL_HANDLE;
};

// colorFormat: 이 파이프라인이 어떤 포맷의 렌더 타겟에 그릴지. 스왑체인에서 온다.
// colorFormat: 이 파이프라인이 어떤 포맷의 렌더 타겟에 그릴지. 스왑체인에서 온다.
Pipeline CreateTrianglePipeline(const VulkanDevice& dev, VkFormat colorFormat) noexcept;
void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept;

// ============================================================================
// 9. 버퍼 - **메모리를 직접 다루는 첫 자리**
// ============================================================================
//
// 지금까지 만든 것(인스턴스·디바이스·스왑체인·커맨드 풀)은 전부 드라이버가 메모리를
// 알아서 잡아줬다. 버퍼부터는 **우리가 어떤 메모리에 놓을지 고른다.**
//
// 그 고르는 과정이 FindMemoryType이고, VMA가 대신해주게 될 부분이 정확히 거기다.
// **한 번은 직접 해봐야 VMA가 무엇을 줄여주는지 알 수 있다.**

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;   // **우리가 할당했다.** 우리가 반납한다
    VkDeviceSize size = 0;
};

// 정점 하나. 셰이더의 layout(location=...) in 과 짝이 맞아야 한다.
//
// GPU가 이 구조체를 어떻게 읽을지는 파이프라인의 vertexInput이 정한다 -
// stride(한 정점의 크기)와 각 필드의 offset·format을 거기서 알려준다.
struct Vertex {
    float position[2];   // vec2 -> VK_FORMAT_R32G32_SFLOAT
    float color[3];      // vec3 -> VK_FORMAT_R32G32B32_SFLOAT
};

// 요구 조건을 만족하는 메모리 타입 번호를 찾는다. 없으면 UINT32_MAX.
//
// **typeBits**: vkGetBufferMemoryRequirements가 준 비트마스크. "이 버퍼는 i번 타입에
//   놓을 수 있다"가 i번 비트로 표현돼 있다. **버퍼가 정하는 제약**이다.
// **required**: 우리가 원하는 성질 (HOST_VISIBLE = CPU가 매핑 가능, DEVICE_LOCAL = GPU 전용 빠른 메모리).
//   **우리가 정하는 요구**다.
//
// 둘을 대조하는 것이 전부다. VMA가 감춰주는 게 이 대조다.
uint32_t FindMemoryType(const VulkanDevice& dev,
                        uint32_t typeBits,
                        VkMemoryPropertyFlags required) noexcept;

// 버퍼 생성 + 메모리 할당 + 바인딩. 셋이 항상 같이 간다.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
Buffer CreateBuffer(const VulkanDevice& dev,
                    VkDeviceSize size,
                    VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags memoryProperties) noexcept;

void DestroyBuffer(const VulkanDevice& dev, Buffer* buffer) noexcept;

// CPU 데이터를 GPU 전용 메모리에 올린다 (스테이징 경유).
//
// **왜 바로 못 올리나**: GPU가 가장 빠르게 읽는 메모리(DEVICE_LOCAL)는 보통 CPU가
// 매핑할 수 없다. 그래서 CPU가 쓸 수 있는 임시 버퍼(스테이징)에 넣고, GPU에게
// "저기서 여기로 복사해"라고 시킨다.
//
// 복사가 끝날 때까지 기다렸다가 스테이징을 버린다 - 초기화 경로라 기다려도 된다.
Buffer CreateVertexBuffer(const VulkanDevice& dev,
                          const Commands& commands,
                          const void* data,
                          VkDeviceSize size) noexcept;
