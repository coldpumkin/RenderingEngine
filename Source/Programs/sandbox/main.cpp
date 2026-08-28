// 한 프레임을 화면에 띄우기까지.
//
// **아직 클래스가 없다. 함수로만 쪼갰다.**
//
// 함수는 소유권 질문("누가 소유하나 · 누가 파괴하나 · 뭘 멤버로 드나")을 묻지 않는다.
// 그 질문에 먼저 답하려다 한 번 길을 잃었기 때문에, 이번엔 순서를 바꿨다:
//
//   1. 함수로 쪼갠다        <- 지금. main()이 목차가 된다
//   2. 시그니처를 본다       <- 무엇이 항상 같이 다니는지가 인자 목록에 드러난다
//   3. 뭉친 것만 클래스로     <- 근거가 생긴 것부터 하나씩
//
// 2번의 힌트는 **out 파라미터**다. 한 함수가 값을 둘 이상 내보내고 있으면
// "이것들은 항상 같이 나오는데 왜 따로 받아가냐"고 코드가 불평하는 것이다.
// 아래 CreateDevice()를 보면 셋을 내보낸다. 그게 첫 번째 클래스가 될 자리다.
//
// 파괴는 아직 전부 main() 끝에 모여 있다. RAII로 옮기는 것은 3번에서 한다.

#include <volk.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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
VkPhysicalDeviceVulkan13Features RequiredFeatures13() noexcept {
    VkPhysicalDeviceVulkan13Features features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.dynamicRendering = VK_TRUE;   // VkRenderPass/VkFramebuffer 없이 그린다
    features.synchronization2 = VK_TRUE;   // 배리어/제출 API 개정판
    return features;
}

// ============================================================================
// 1. 인스턴스 (+ 디버그 메신저)
// ============================================================================
//
// 둘을 한 함수에서 만드는 이유: **같이 태어나고 같이 죽는다.** 메신저는 인스턴스가
// 없으면 만들 수 없고, 인스턴스가 죽으면 의미가 없다.

#if LAMBDA_ENABLE_VULKAN_VALIDATION

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// 반환값은 "이 Vulkan 호출을 중단시킬까"를 뜻한다. 레이어 자체를 시험하는 게 아니면
// 항상 VK_FALSE여야 한다.
//
// 접두어가 [Vulkan]인 이유: 우리가 찍는 게 아니라 레이어가 알려주는 것이고,
// **우리 코드가 아닌 것도 여기로 들어온다** - Steam/OBS 오버레이가 시스템에 등록한
// 암묵적 레이어의 경고가 섞인다. 내 것만 보려면 실행할 때 환경변수로 거른다:
//   $env:VK_LOADER_LAYERS_DISABLE="~implicit~"
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) noexcept {

    const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR"
                      : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN"
                      : "INFO";
    LOG("[Vulkan %s] %s\n", level, data->pMessage != nullptr ? data->pMessage : "(no message)");
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MakeMessengerInfo() noexcept {
    VkDebugUtilsMessengerCreateInfoEXT info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    // VERBOSE/INFO는 지금 단계에선 소음이 커서 뺀다.
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugCallback;
    return info;
}

// vkEnumerateInstanceLayerProperties는 volkInitialize()가 채우는 몇 안 되는 함수 중 하나다.
// 인스턴스가 없어도 부를 수 있어야 해서 부트스트랩에 포함돼 있고, 덕분에
// "무엇을 켤 수 있는지 먼저 조사한 다음 인스턴스를 만든다"가 가능하다.
bool HasInstanceLayer(const char* name) noexcept {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) { return false; }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) { return false; }
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) { return true; }
    }
    return false;
}

#endif // LAMBDA_ENABLE_VULKAN_VALIDATION

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
VulkanInstance CreateInstance() noexcept {
    VulkanInstance inst;

    // volk: vulkan-1.dll을 런타임에 LoadLibrary로 찾는다. 없는 기계에서도 프로세스가
    // 죽지 않고 여기서 실패를 돌려받는다. (Vulkan::Vulkan을 정적 링크했다면 시작조차 못 했다.)
    if (volkInitialize() != VK_SUCCESS) {
        LOG("[vk] volkInitialize failed (vulkan-1.dll not found?)\n");
        return inst;
    }

    // vkEnumerateInstanceVersion은 Vulkan 1.1에서 추가됐다. 1.0 로더에서는 volk가 이
    // 포인터를 못 채우므로, **nullptr이라는 사실 자체가 "이 기계는 1.0"이라는 정보다.**
    if (vkEnumerateInstanceVersion == nullptr) {
        LOG("[vk] Vulkan 1.0 loader; need 1.3\n");
        return inst;
    }
    uint32_t loaderVersion = 0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < kRequiredApiVersion) {
        LOG("[vk] loader is %u.%u; need 1.3\n",
            VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return inst;
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "LambdaEngine";
    appInfo.apiVersion = kRequiredApiVersion;

    // 서피스 확장은 선택이 아니다. 창에 그림을 못 내보내면 이 엔진은 할 일이 없다.
    // VK_KHR_surface는 플랫폼 공통, VK_KHR_win32_surface는 "HWND로 서피스를 만드는 법"이다.
    std::vector<const char*> extensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    std::vector<const char*> layers;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &appInfo;

#if LAMBDA_ENABLE_VULKAN_VALIDATION
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeMessengerInfo();
    bool validationOn = false;
    if (HasInstanceLayer(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        // pNext에 걸면 **vkCreateInstance/vkDestroyInstance 자체의 문제도 잡힌다.**
        // 메신저를 따로 만들기만 하면 그 사이 구간이 감시 밖에 남는다.
        info.pNext = &messengerInfo;
        validationOn = true;
    } else {
        LOG("[vk] validation layer not available (Vulkan SDK installed?)\n");
    }
#endif

    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&info, nullptr, &inst.handle) != VK_SUCCESS) {
        LOG("[vk] vkCreateInstance failed\n");
        return inst;
    }

    // **인스턴스 레벨도 테이블로 받는다.** 디바이스 테이블과 같은 이유다 -
    // 핸들과 함수가 한 곳에서 나와야 섞일 수 없다.
    volkLoadInstanceTable(&inst.table, inst.handle);

    // **그런데 전역도 같이 채워야 한다.** volkLoadDeviceTable()이 내부에서
    // 전역 vkGetDeviceProcAddr를 쓰기 때문이다 (volk.c:72-75, 224-228).
    // volk의 한계라 우리가 없앨 수 없다. 그래서 인스턴스 레벨을 테이블로 쓰는 것은
    // **강제가 아니라 규약**이다 - 실수로 전역을 불러도 조용히 동작한다.
    //
    // volkLoadInstance가 아니라 volkLoadInstanceOnly인 것에 주의: 전자는 디바이스 레벨
    // 포인터까지 전역에 채워서, 디바이스 테이블을 안 쓰고 전역으로 불러도 동작하게 된다.
    // 멀티 디바이스에서 그건 틀린 디바이스를 부르는 버그가 되고, 조용해서 안 잡힌다.
    volkLoadInstanceOnly(inst.handle);

#if LAMBDA_ENABLE_VULKAN_VALIDATION
    if (validationOn) {
        inst.table.vkCreateDebugUtilsMessengerEXT(inst.handle, &messengerInfo, nullptr,
                                                  &inst.messenger);
    }
#endif

    LOG("[vk] instance created (Vulkan 1.3 requested, loader %u.%u)\n",
        VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
    return inst;
}

// ============================================================================
// 2. 창 시스템 (프로세스 하나당) + 창 (창마다)
// ============================================================================
//
// 둘을 나눈 이유: **glfwInit/glfwTerminate는 창이 아니라 프로세스 단위다.**
// 창이 둘이 돼도 초기화는 한 번이고, glfwTerminate는 **모든** 창을 부순다.
// 한 함수에 섞여 있으면 창이 둘 될 때 바로 어긋난다.

void OnGlfwError(int code, const char* description) {
    LOG("[glfw] error %d: %s\n", code, description);
}

bool InitWindowSystem() noexcept {
    // 에러 콜백을 glfwInit보다 먼저 건다. glfwInit 자체의 실패 이유도 받으려면 그래야 한다
    // (GLFW 문서가 명시하는, 초기화 전에 부를 수 있는 예외 함수).
    glfwSetErrorCallback(OnGlfwError);
    if (glfwInit() != GLFW_TRUE) {
        LOG("[glfw] glfwInit failed\n");
        return false;
    }
    return true;
}

void ShutdownWindowSystem() noexcept {
    glfwTerminate();
}

// ---------------------------------------------------------------------------
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
void OnFramebufferResized(GLFWwindow* handle, int /*w*/, int /*h*/) {
    auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (window != nullptr) {
        window->swapchainOutOfDate = true;
    }
}

// 창 + 서피스까지 만든다. 스왑체인은 디바이스가 생긴 뒤라 여기서 못 만든다.
//
// **out으로 받는 이유**: glfwSetWindowUserPointer에 넣을 주소가 **호출자가 들고 있을
// 최종 주소**여야 한다. 값으로 반환하면 함수 안의 임시 객체 주소를 넣게 된다.
bool OpenWindow(const VulkanInstance& inst,
                int width, int height, const char* title,
                Window* out) noexcept {
    *out = Window{};

    // GLFW는 기본적으로 OpenGL 컨텍스트를 같이 만든다. Vulkan을 쓰므로 끈다.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    out->handle = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (out->handle == nullptr) {
        LOG("[glfw] glfwCreateWindow failed\n");
        return false;
    }
    glfwSetWindowUserPointer(out->handle, out);
    glfwSetFramebufferSizeCallback(out->handle, OnFramebufferResized);

    // glfwCreateWindowSurface()도 있지만 쓰지 않는다. 직접 만들면 **창 라이브러리와
    // Vulkan이 서로를 모르는 상태로 남는다** - GLFW가 VkInstance를 알 필요가 없다.
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = glfwGetWin32Window(out->handle);

    if (inst.table.vkCreateWin32SurfaceKHR(inst.handle, &info, nullptr, &out->surface)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateWin32SurfaceKHR failed\n");
        glfwDestroyWindow(out->handle);
        *out = Window{};
        return false;
    }
    return true;
}

// ============================================================================
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
bool SelectQueueFamilies(const VulkanInstance& inst,
                         VkPhysicalDevice gpu,
                         QueueFamilies* out) noexcept {
    *out = QueueFamilies{};

    uint32_t count = 0;
    inst.table.vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    inst.table.vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());

    // (1) 그래픽스 + present. **서피스가 아니라 플랫폼에 묻는다** -
    //     vkGetPhysicalDeviceWin32PresentationSupportKHR은 "이 큐 패밀리가 Win32
    //     데스크톱에 present 할 수 있는가"를 답하고, 창이 없어도 부를 수 있다.
    //     VkBool32를 돌려주는 것에 주목 - 실패할 수 있는 조회가 아니라 성질이다.
    //
    //     (이 함수는 Win32/Wayland/Xcb/Xlib에만 있다. Android/iOS/macOS엔 없어서
    //      언리얼은 "서피스가 생긴 뒤 확인"하는 2단계 초기화를 쓴다.)
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) { continue; }
        if (inst.table.vkGetPhysicalDeviceWin32PresentationSupportKHR(gpu, i) != VK_TRUE) {
            continue;
        }
        out->graphics = i;
        break;
    }
    if (out->graphics == UINT32_MAX) { return false; }

    // (2) 전용 컴퓨트: COMPUTE는 있고 GRAPHICS는 없는 패밀리.
    //     GRAPHICS가 없다는 조건 하나로 "그래픽스 패밀리와 다르다"가 자동 보장된다.
    for (uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) == 0) { continue; }
        if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0) { continue; }
        out->compute = i;
        break;
    }

    // (3) 전용 전송: TRANSFER는 있고 GRAPHICS도 COMPUTE도 없는 패밀리.
    //     **여기서만 TRANSFER 비트를 본다** - "전송을 할 수 있나"가 아니라
    //     "전송만 하는 전용 엔진인가"를 묻는 것이라서다.
    for (uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) == 0) { continue; }
        if ((flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0) { continue; }
        out->transfer = i;
        break;
    }

    return true;
}

// 고르기의 결과. **수명이 없는 값 타입이다.**
//
// vkDestroyPhysicalDevice 같은 함수는 존재하지 않는다 - GPU는 우리가 만든 게 아니라
// 열거해서 고른 것이라 반납할 게 없다. 그래서 이건 영원히 struct고 클래스가 될 일이 없다.
//
// 지나가는 값이다: PickPhysicalDevice가 만들고, CreateDevice가 소비해서
// **VulkanDevice 안으로 흡수된다.** 그 뒤로 따로 들고 있지 않는다.
struct PhysicalDeviceSelection {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    QueueFamilies families;
};

// GPU 고르기. 자격을 통과한 것 중 외장을 선호한다.
// 실패하면 gpu가 VK_NULL_HANDLE인 채로 돌아온다.
PhysicalDeviceSelection PickPhysicalDevice(const VulkanInstance& inst,
                                           VkSurfaceKHR surface) noexcept {
    PhysicalDeviceSelection selection;

    uint32_t gpuCount = 0;
    inst.table.vkEnumeratePhysicalDevices(inst.handle, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG("[vk] no Vulkan-capable GPU\n");
        return selection;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    inst.table.vkEnumeratePhysicalDevices(inst.handle, &gpuCount, gpus.data());

    VkPhysicalDeviceProperties chosenProps{};
    int bestScore = -1;

    for (VkPhysicalDevice candidate : gpus) {
        VkPhysicalDeviceProperties props{};
        inst.table.vkGetPhysicalDeviceProperties(candidate, &props);

        // (a) API 버전
        if (props.apiVersion < kRequiredApiVersion) { continue; }

        // (b) 1.3 기능이 실제로 켜져 있는가 (버전을 지원해도 꺼져 있을 수 있다)
        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &features13;
        inst.table.vkGetPhysicalDeviceFeatures2(candidate, &features2);
        if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE) {
            continue;
        }

        // (c) 스왑체인 확장
        uint32_t extCount = 0;
        inst.table.vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        inst.table.vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, available.data());

        bool hasAllExtensions = true;
        for (const char* required : kRequiredDeviceExtensions) {
            bool found = false;
            for (const VkExtensionProperties& ext : available) {
                if (std::strcmp(ext.extensionName, required) == 0) { found = true; break; }
            }
            if (!found) { hasAllExtensions = false; break; }
        }
        if (!hasAllExtensions) { continue; }

        // (d) 큐 패밀리 - 그래픽스+present가 없으면 탈락. 컴퓨트/전송은 있으면 좋고 없어도 된다.
        QueueFamilies families;
        if (!SelectQueueFamilies(inst, candidate, &families)) { continue; }

        const int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000 : 0;
        if (score > bestScore) {
            bestScore = score;
            selection.gpu = candidate;
            selection.families = families;
            chosenProps = props;
        }
    }

    if (selection.gpu == VK_NULL_HANDLE) {
        LOG("[vk] no GPU meets requirements (1.3 + dynamicRendering + sync2 + swapchain)\n");
        return selection;
    }

    // 고른 큐 패밀리가 **이 서피스**에도 present 되는지 확인한다.
    // 위의 Win32 확인은 "플랫폼에 대해"이고 이건 "이 창에 대해"다.
    // 층이 다르고 서로를 대신하지 않는다.
    VkBool32 surfaceSupported = VK_FALSE;
    inst.table.vkGetPhysicalDeviceSurfaceSupportKHR(selection.gpu, selection.families.graphics,
                                                    surface, &surfaceSupported);
    if (surfaceSupported != VK_TRUE) {
        selection.gpu = VK_NULL_HANDLE;   // 실패는 gpu가 비어 있는 것으로 표현한다
        LOG("[vk] chosen queue family cannot present to this surface\n");
        return selection;
    }

    LOG("[vk] GPU: %s\n", chosenProps.deviceName);
    LOG("[vk] queue families: graphics=%u, compute=%s, transfer=%s\n",
        selection.families.graphics,
        selection.families.HasCompute()  ? std::to_string(selection.families.compute).c_str()  : "(none, use graphics)",
        selection.families.HasTransfer() ? std::to_string(selection.families.transfer).c_str() : "(none, use graphics)");
    return selection;
}

// ============================================================================
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
};

// 논리 디바이스 + 함수 테이블 + 큐들.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// **inst를 받는 이유**: vkCreateDevice는 **인스턴스 레벨 함수**다. 만드는 함수와
// 파괴하는 함수(vkDestroyDevice, 디바이스 레벨)의 층이 다르다는 Vulkan API의 비대칭이고,
// 그래서 "이 클래스가 무슨 레벨이냐"가 아니라 "이 호출이 무슨 레벨이냐"로 봐야 한다.
VulkanDevice CreateDevice(const VulkanInstance& inst,
                          const PhysicalDeviceSelection& selection) noexcept {
    VulkanDevice dev;
    dev.gpu = selection.gpu;
    dev.families = selection.families;
    const QueueFamilies& families = dev.families;

    // 스펙: pQueueCreateInfos 안의 queueFamilyIndex는 **서로 달라야 한다.**
    // SelectQueueFamilies가 "GRAPHICS 없는 것만 compute", "GRAPHICS/COMPUTE 없는 것만
    // transfer"로 골랐으므로 셋은 자동으로 서로 다르다. 중복 제거가 필요 없다.
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfos[3]{};
    uint32_t queueInfoCount = 0;

    auto addQueue = [&](uint32_t family) {
        VkDeviceQueueCreateInfo& q = queueInfos[queueInfoCount++];
        q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex = family;
        q.queueCount = 1;              // 패밀리당 하나면 지금은 충분하다
        q.pQueuePriorities = &priority;
    };

    addQueue(families.graphics);
    if (families.HasCompute())  { addQueue(families.compute); }
    if (families.HasTransfer()) { addQueue(families.transfer); }

    // 지원 여부를 확인만 하는 게 아니라 **켜달라고 요청**해야 쓸 수 있다.
    VkPhysicalDeviceVulkan13Features enable13 = RequiredFeatures13();

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &enable13;
    info.queueCreateInfoCount = queueInfoCount;
    info.pQueueCreateInfos = queueInfos;
    info.enabledExtensionCount = static_cast<uint32_t>(std::size(kRequiredDeviceExtensions));
    info.ppEnabledExtensionNames = kRequiredDeviceExtensions;

    // **inst.table을 거친다.** 여기가 한동안 전역 vkCreateDevice를 부르고 있었다 -
    // 동작은 했지만(volkLoadInstanceOnly가 전역을 채워둬서) 테이블 규약 위반이었다.
    if (inst.table.vkCreateDevice(dev.gpu, &info, nullptr, &dev.handle) != VK_SUCCESS) {
        LOG("[vk] vkCreateDevice failed\n");
        return dev;
    }

    // **전역이 아니라 테이블로 받는다.** 전역(volkLoadDevice)은 마지막으로 로드한
    // 디바이스로 덮인다. 디바이스가 둘이 되는 순간 조용히 틀린 디바이스를 부르게 되고,
    // 조용해서 안 잡힌다. 지금 디바이스는 하나지만 **테이블을 쓰면 그 버그가 아예
    // 표현 불가능해진다.**
    volkLoadDeviceTable(&dev.table, dev.handle);

    // vkGetDeviceQueue는 **조회**다. vkCreateDevice가 이미 만들었고, 파괴 함수도 없다.
    dev.table.vkGetDeviceQueue(dev.handle, families.graphics, 0, &dev.queues.graphics);
    if (families.HasCompute()) {
        dev.table.vkGetDeviceQueue(dev.handle, families.compute, 0, &dev.queues.compute);
    }
    if (families.HasTransfer()) {
        dev.table.vkGetDeviceQueue(dev.handle, families.transfer, 0, &dev.queues.transfer);
    }

    // present는 새로 만드는 게 아니라 위에서 만든 것 중 하나를 가리킨다.
    // 그래픽스 패밀리가 present를 지원하는 것은 PickPhysicalDevice가 이미 확인했다.
    dev.queues.present = dev.queues.graphics;

    return dev;
}

// ============================================================================
// 5. 스왑체인 - **여기만 수명이 있다**
// ============================================================================
//
// 다른 것들은 전부 한 번 만들고 끝까지 간다. 스왑체인만 창 크기가 바뀔 때마다
// 다시 만든다. **"같이 바뀌는가" 축에서 유일하게 혼자 움직이는 덩어리**라
// 클래스로 뺄 근거가 이미 가장 뚜렷하다.

// SRGB를 우선하는 이유: 모니터는 선형이 아니라 감마 곡선으로 빛을 낸다. 포맷에 _SRGB가
// 붙어 있으면 GPU가 그 변환을 하드웨어로 해준다. UNORM을 쓰면 셰이더에서 직접 감마
// 보정을 해야 하고, 안 하면 화면이 어둡게 나온다.
VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) noexcept {
    for (const VkSurfaceFormatKHR& f : available) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB
            && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return available.front();   // 스펙상 목록은 최소 하나가 보장된다
}

// OPAQUE = "알파를 무시하고 불투명하게 합성하라". 창 투명도를 안 쓰므로 이게 맞다.
//
// **그래도 확인하고 고른다.** FIFO와 달리 이건 스펙이 지원을 보장하지 않는다.
// 지원 안 하는 값을 박아 넣으면 vkCreateSwapchainKHR이 실패하는데, 그 실패 코드만으로는
// 무엇이 문제인지 알 수 없다.
VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) noexcept {
    constexpr VkCompositeAlphaFlagBitsKHR kPreferred[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR candidate : kPreferred) {
        if ((supported & candidate) != 0) { return candidate; }
    }
    return static_cast<VkCompositeAlphaFlagBitsKHR>(0);   // 드라이버가 스펙을 어긴 경우
}

void DestroySwapchain(const VulkanDevice& dev, Swapchain* sc) noexcept {
    if (sc->handle == VK_NULL_HANDLE) { return; }

    // GPU가 아직 이 이미지들을 쓰고 있을 수 있다. 스펙상 사용 중인 오브젝트 파괴는 금지다.
    dev.table.vkDeviceWaitIdle(dev.handle);

    for (SwapchainImage& img : sc->images) {
        dev.table.vkDestroySemaphore(dev.handle, img.renderFinished, nullptr);
        dev.table.vkDestroyImageView(dev.handle, img.view, nullptr);
        // img.image는 파괴하지 않는다 - vkGetSwapchainImagesKHR로 **조회**한 것이고
        // 스왑체인이 소유한다.
    }
    sc->images.clear();

    dev.table.vkDestroySwapchainKHR(dev.handle, sc->handle, nullptr);
    *sc = Swapchain{};
}

// 실패 또는 "지금은 만들 수 없음"(최소화)이면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// oldSwapchain을 넘겨야 하는 이유: **한 서피스에 스왑체인 둘이 동시에 존재할 수 없다.**
// 안 넘기면 재생성 자체가 실패한다. 넘기면 그 순간 이전 것은 "은퇴" 상태가 되고,
// **파괴는 여전히 우리 몫이다.**
//
// **인자가 다섯이고 테이블이 둘이다.** 서피스 조회는 인스턴스 레벨(it), 스왑체인 생성은
// 디바이스 레벨(vk)이라 양쪽이 다 필요하다. 스왑체인이 두 층의 경계에 서 있다는 뜻이고,
// 클래스가 되면 그 경계가 인자 둘로 줄어든다 (2단계 증거).
Swapchain CreateSwapchain(const VulkanInstance& inst,
                          const VulkanDevice& dev,
                          VkSurfaceKHR surface,
                          VkSwapchainKHR oldSwapchain) noexcept {
    Swapchain sc;

    // ---- 서피스에게 "이 창은 무엇을 받나"를 묻는다 ----
    VkSurfaceCapabilitiesKHR caps{};
    if (inst.table.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.gpu, surface, &caps) != VK_SUCCESS) {
        LOG("[vk] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed\n");
        return sc;
    }

    // 창이 최소화되면 서피스 크기가 0x0이 된다. 스왑체인을 만들 수 없지만 **오류가 아니라
    // 정상 상황**이고, 최소화가 풀릴 때까지 지속된다. 그래서 로그를 찍지 않는다 -
    // 찍으면 최소화하고 있는 내내 초당 수백 줄이 된다.
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
        return sc;
    }

    uint32_t formatCount = 0;
    inst.table.vkGetPhysicalDeviceSurfaceFormatsKHR(dev.gpu, surface, &formatCount, nullptr);
    if (formatCount == 0) {
        LOG("[vk] surface reports no formats\n");
        return sc;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    inst.table.vkGetPhysicalDeviceSurfaceFormatsKHR(dev.gpu, surface, &formatCount, formats.data());

    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(formats);

    const VkCompositeAlphaFlagBitsKHR compositeAlpha =
        ChooseCompositeAlpha(caps.supportedCompositeAlpha);
    if (compositeAlpha == 0) {
        LOG("[vk] no usable composite alpha (0x%x)\n", caps.supportedCompositeAlpha);
        return sc;
    }

    // 이미지 개수: 최소보다 하나 더 요청한다. 최소만 요청하면 드라이버가 다음 이미지를
    // 내줄 때까지 매번 기다리게 된다. maxImageCount == 0은 "상한 없음"이라 클램프에서 뺀다.
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat.format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = caps.currentExtent;
    info.imageArrayLayers = 1;
    // COLOR_ATTACHMENT = "여기에 직접 그린다". 나중에 오프스크린에 그리고 복사만 하게 되면
    // TRANSFER_DST를 추가하게 된다.
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // EXCLUSIVE로 두는 이유: **스왑체인 이미지는 그래픽스 큐만 만진다** (그리고, present한다).
    // 컴퓨트/전송 큐가 따로 있어도 이 이미지에는 손대지 않는다. 나중에 컴퓨트가 스왑체인
    // 이미지에 직접 써야 하면 그때 CONCURRENT로 바꾸거나 큐 패밀리 소유권 이전을 넣는다
    // - **둘 다 비용이 있으니 필요해질 때 고른다.**
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = compositeAlpha;
    // FIFO는 **스펙이 항상 지원을 보장하는 유일한 모드**다. 수직동기와 같아 티어링이 없다.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    const VkResult created = dev.table.vkCreateSwapchainKHR(dev.handle, &info, nullptr, &sc.handle);
    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateSwapchainKHR failed (%d)\n", created);
        sc.handle = VK_NULL_HANDLE;
        return sc;
    }

    sc.format = surfaceFormat.format;
    sc.extent = caps.currentExtent;

    // ---- 이미지 조회 ----
    // **개수는 요청이 아니라 결과다.** minImageCount는 최소일 뿐이고 드라이버가 더 줄 수
    // 있다. 만든 뒤에 실제 개수를 다시 물어야 한다.
    uint32_t actualCount = 0;
    if (dev.table.vkGetSwapchainImagesKHR(dev.handle, sc.handle, &actualCount, nullptr)
            != VK_SUCCESS || actualCount == 0) {
        LOG("[vk] vkGetSwapchainImagesKHR returned no images\n");
        DestroySwapchain(dev, &sc);
        return sc;
    }
    std::vector<VkImage> rawImages(actualCount);
    if (dev.table.vkGetSwapchainImagesKHR(dev.handle, sc.handle, &actualCount, rawImages.data())
            != VK_SUCCESS) {
        LOG("[vk] vkGetSwapchainImagesKHR failed\n");
        DestroySwapchain(dev, &sc);
        return sc;
    }

    // ---- 이미지마다 뷰 + 세마포어 ----
    //
    // **여기서 실패하면 통째로 버린다.** 반만 만들어진 스왑체인을 성공으로 돌려주면
    // 호출자는 sc.handle이 유효한 것만 보고 다음 프레임에 null 뷰로 렌더링을 시도한다.
    // "객체가 존재하면 항상 유효하다"를 지키려면 중간 실패에서 되돌려야 한다.
    //
    // 되돌리기는 DestroySwapchain이 그대로 해준다 - vkDestroy~는 VK_NULL_HANDLE에
    // 대해 no-op이라(스펙 보장) 반쯤 채워진 배열도 안전하게 정리된다.
    sc.images.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        sc.images[i].image = rawImages[i];

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = rawImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = sc.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        const VkResult viewResult =
            dev.table.vkCreateImageView(dev.handle, &viewInfo, nullptr, &sc.images[i].view);
        if (viewResult != VK_SUCCESS) {
            LOG("[vk] vkCreateImageView failed on image %u (%d)\n", i, viewResult);
            DestroySwapchain(dev, &sc);
            return sc;
        }

        // **이미지당 하나인 이유**: 이 세마포어는 present가 기다린다. 그런데 present에는
        // 완료를 알려주는 것이 없다 - vkQueuePresentKHR은 펜스를 주지 않는다.
        // "다시 signal해도 된다"는 유일한 단서가 **acquire가 그 이미지를 다시 줬다**는
        // 사실이고, 그 단서는 이미지 인덱스로만 온다. 그래서 개수도 이미지 수다.
        //
        // (반대로 imageAvailable은 acquire를 부르기 전에는 인덱스를 모르므로 이미지당으로
        //  둘 수 없다. 정확히 반대 방향이고, 이 비대칭이 "스왑체인에 묶인 자원"과
        //  "프레임에 묶인 자원"을 가르는 선이다.)
        //
        // 완전한 해법은 VK_KHR_swapchain_maintenance1의 VkSwapchainPresentFenceInfoKHR다 -
        // present에 펜스를 붙일 수 있다. **그 확장이 따로 생겼다는 사실 자체가
        // 원래 present에 완료 신호가 없었다는 증거다.** 지금은 필요 없다.
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        const VkResult semResult =
            dev.table.vkCreateSemaphore(dev.handle, &semInfo, nullptr,
                                        &sc.images[i].renderFinished);
        if (semResult != VK_SUCCESS) {
            LOG("[vk] vkCreateSemaphore failed on image %u (%d)\n", i, semResult);
            DestroySwapchain(dev, &sc);
            return sc;
        }
    }

    LOG("[vk] swapchain %ux%u, %u images, format %d, FIFO\n",
        sc.extent.width, sc.extent.height, actualCount, static_cast<int>(sc.format));
    return sc;
}

// ---------------------------------------------------------------------------
// 창 단위 연산 - 스왑체인이 있어야 성립하므로 여기(5절 끝)에 있다
// ---------------------------------------------------------------------------

// 그릴 곳을 보장한다. 낡았거나 없으면 다시 만든다.
// **false는 실패가 아니라 "지금은 그릴 곳이 없다"** (최소화 중)이다.
//
// 루프의 0단계가 통째로 여기 들어왔다. 재생성 조건 판단, oldSwapchain 넘기기,
// 이전 것 파괴가 한 덩어리라 흩어져 있을 이유가 없다.
bool EnsureSwapchain(const VulkanInstance& inst,
                     const VulkanDevice& dev,
                     Window* window) noexcept {
    if (!window->swapchainOutOfDate && window->swapchain.handle != VK_NULL_HANDLE) {
        return true;
    }

    dev.table.vkDeviceWaitIdle(dev.handle);

    // 이전 것을 oldSwapchain으로 넘겨 **은퇴시키고**, 새것을 만든 뒤에 파괴한다.
    // 넘긴 것은 "은퇴시켜라"는 뜻이지 "네가 지워라"가 아니다 - 파괴는 여전히 우리 몫이다.
    // (생성이 실패해도 은퇴는 일어나므로, 실패해도 이전 것은 버려야 한다.)
    Swapchain fresh =
        CreateSwapchain(inst, dev, window->surface, window->swapchain.handle);
    DestroySwapchain(dev, &window->swapchain);

    window->swapchain = std::move(fresh);
    window->swapchainOutOfDate = false;

    return window->swapchain.handle != VK_NULL_HANDLE;
}

// 중첩의 역순으로 부순다: 스왑체인 -> 서피스 -> 창.
//
// **서피스가 창보다 먼저 죽어야 한다** - 죽은 HWND를 참조하게 된다.
// 이 순서가 한 함수 안에 있는 것이 이 묶음의 값이다. 전에는 main() 끝에
// 다른 정리들과 섞여 있어서 순서를 틀리기 쉬웠다.
void CloseWindow(const VulkanInstance& inst, const VulkanDevice& dev, Window* window) noexcept {
    DestroySwapchain(dev, &window->swapchain);

    if (window->surface != VK_NULL_HANDLE) {
        inst.table.vkDestroySurfaceKHR(inst.handle, window->surface, nullptr);
    }
    if (window->handle != nullptr) {
        glfwDestroyWindow(window->handle);
    }
    *window = Window{};
}

// ============================================================================
// 6. 커맨드 풀 - **큐 패밀리마다 하나**
// ============================================================================
//
// 스펙 제약이라 선택의 여지가 없다: 풀은 queueFamilyIndex로 만들어지고,
// **그 풀에서 나온 커맨드 버퍼는 같은 패밀리의 큐에만 제출할 수 있다.**
// 컴퓨트 큐에 뭔가 제출하려면 컴퓨트 패밀리의 풀이 반드시 있어야 한다.
//
// **풀의 개수는 세 축의 곱이다:**
//
//   큐 패밀리        위 제약. 지금 3 (graphics/compute/transfer)
//   스레드           풀은 스레드 안전이 아니다(외부 동기화 필요). 지금 1
//   frames-in-flight 프레임 단위로 통째 리셋하려면 프레임마다 따로. 지금 1
//
// 그래서 지금은 3 x 1 x 1 = 3개다. **frames-in-flight를 2로 올리면 6개가 된다** -
// 이 곱셈이 나중에 "풀을 무엇으로 묶을 것인가"를 정한다.

// 풀 하나와 거기서 뽑은 버퍼 하나. 같이 태어나고 같이 죽는다
// (버퍼를 따로 반납하지 않는다 - 풀을 파괴하면 같이 사라진다).
struct CommandSet {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
};

// 전용 패밀리가 없는 큐의 CommandSet은 비어 있다(pool == VK_NULL_HANDLE).
// 그때 그 일은 graphics 것으로 한다.
struct Commands {
    CommandSet graphics;
    CommandSet compute;
    CommandSet transfer;
};

CommandSet CreateCommandSet(const VulkanDevice& dev, uint32_t queueFamily) noexcept {
    CommandSet set;

    // RESET_COMMAND_BUFFER: 풀 전체가 아니라 버퍼 하나만 개별 리셋할 수 있게 한다.
    // frames-in-flight가 늘어 프레임 단위로 리셋하게 되면 이 플래그를 빼고
    // vkResetCommandPool을 쓰는 쪽이 빨라진다 - 그때 다시 본다.
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;

    if (dev.table.vkCreateCommandPool(dev.handle, &poolInfo, nullptr, &set.pool) != VK_SUCCESS) {
        LOG("[vk] vkCreateCommandPool failed (family %u)\n", queueFamily);
        return CommandSet{};
    }

    // PRIMARY: 큐에 직접 제출할 수 있다. SECONDARY는 다른 버퍼 안에서만 실행된다.
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = set.pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    // 실패하면 풀만 남고 버퍼는 VK_NULL_HANDLE인 CommandSet이 나간다 - 호출자는
    // pool만 보고 성공으로 읽으므로, 여기서 되돌려야 "존재하면 유효"가 지켜진다.
    const VkResult allocResult =
        dev.table.vkAllocateCommandBuffers(dev.handle, &allocInfo, &set.buffer);
    if (allocResult != VK_SUCCESS) {
        LOG("[vk] vkAllocateCommandBuffers failed (%d)\n", allocResult);
        dev.table.vkDestroyCommandPool(dev.handle, set.pool, nullptr);
        return CommandSet{};
    }

    return set;
}

// 있는 패밀리마다 하나씩. 그래픽스는 필수, 나머지는 전용 패밀리가 있을 때만.
bool CreateCommands(const VulkanDevice& dev, Commands* out) noexcept {
    *out = Commands{};

    out->graphics = CreateCommandSet(dev, dev.families.graphics);
    if (out->graphics.pool == VK_NULL_HANDLE) { return false; }

    if (dev.families.HasCompute()) {
        out->compute = CreateCommandSet(dev, dev.families.compute);
        if (out->compute.pool == VK_NULL_HANDLE) { return false; }
    }
    if (dev.families.HasTransfer()) {
        out->transfer = CreateCommandSet(dev, dev.families.transfer);
        if (out->transfer.pool == VK_NULL_HANDLE) { return false; }
    }
    return true;
}

void DestroyCommands(const VulkanDevice& dev, Commands* c) noexcept {
    // 버퍼는 따로 반납하지 않는다. 풀을 파괴하면 같이 사라진다.
    for (CommandSet* set : {&c->graphics, &c->compute, &c->transfer}) {
        if (set->pool != VK_NULL_HANDLE) {
            dev.table.vkDestroyCommandPool(dev.handle, set->pool, nullptr);
        }
    }
    *c = Commands{};
}

// ============================================================================
// 7. 한 프레임 기록하기
// ============================================================================

// 이미지 레이아웃 전이. 프레임에 두 번 나오는데 방향만 다르다.
//
// GPU 이미지는 **용도마다 내부 배치가 다르다.** "렌더 타겟으로 쓸 때 빠른 배치"와
// "화면에 내보낼 때의 배치"가 다르고, 그 사이를 명시적으로 바꿔줘야 한다.
// 배리어는 그 전환과 함께 **메모리 가시성**(앞의 쓰기가 뒤의 읽기에 보이는가)도 처리한다.
void RecordLayoutTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd, VkImage image,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                            VkImageLayout oldLayout, VkImageLayout newLayout) noexcept {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vk.vkCmdPipelineBarrier2(cmd, &dep);
}

// 프레임의 6~11단계: 배리어 -> 렌더링 시작 -> **드로우** -> 렌더링 끝 -> 배리어 -> 기록 끝.
//
// **인자가 넷뿐인 것에 주목.** 동기화(펜스·세마포어·acquire·present)가 하나도 안 들어온다 -
// 그건 전부 main()의 루프에 남아 있다. 기록과 동기화는 서로 모르는 채로 돌아간다.
// 이게 나중에 "기록하는 쪽"과 "제출하는 쪽"이 갈리는 선이다.
void RecordFrame(const VolkDeviceTable& vk,
                 VkCommandBuffer cmd,
                 const SwapchainImage& target,
                 VkExtent2D extent) noexcept {
    vk.vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    // ONE_TIME_SUBMIT: 한 번 제출하고 버릴 기록이라고 드라이버에 알린다.
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(cmd, &beginInfo);

    // ---- 그릴 수 있는 레이아웃으로 ----
    // oldLayout이 UNDEFINED인 것은 이전 내용을 안 쓰기 때문이다 - 어차피 loadOp=CLEAR로
    // 덮는다. 보존을 요구하면 드라이버가 실제로 복사를 해야 한다.
    RecordLayoutTransition(vk, cmd, target.image,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // ---- 렌더링 시작 ----
    // 다이나믹 렌더링: VkRenderPass/VkFramebuffer 객체를 미리 만들지 않는다.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vk.vkCmdBeginRendering(cmd, &rendering);

    // ======== 드로우콜이 들어올 자리 ========

    vk.vkCmdEndRendering(cmd);

    // ---- present 가능한 레이아웃으로 ----
    RecordLayoutTransition(vk, cmd, target.image,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    vk.vkEndCommandBuffer(cmd);
}

// ============================================================================
// main - 목차
// ============================================================================
int main() {
    // ---- 만든다 (위 함수들 순서대로) ----
    //
    // out 파라미터가 하나도 없다. 묶음이 곧 반환값이다.
    const VulkanInstance inst = CreateInstance();
    if (inst.handle == VK_NULL_HANDLE) { return 1; }

    if (!InitWindowSystem()) { return 1; }

    // 창 + 서피스. 스왑체인은 디바이스가 생긴 뒤라 아직 비어 있다(= 최소화와 같은 정상 상태).
    Window window;
    if (!OpenWindow(inst, 1280, 720, "Lambda Engine", &window)) { return 1; }

    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    // selection은 여기서 dev 안으로 흡수되고 더 이상 쓰이지 않는다.
    const VulkanDevice dev = CreateDevice(inst, selection);
    if (dev.handle == VK_NULL_HANDLE) { return 1; }

    // 스왑체인은 루프의 EnsureSwapchain이 만든다 - 최초 생성도 재생성과 같은 경로다.
    // 특별 취급을 없앴다: "지금 그릴 곳이 없다"가 시작 시점에도 정상 상태이기 때문이다
    // (최소화된 채로 실행할 수 있다).

    // 있는 패밀리마다 풀 하나씩. 컴퓨트/전송 풀은 **아직 아무것도 제출하지 않는다** -
    // 만들어만 두고 실제 작업(업로드, 디스패치)이 생길 때 쓴다.
    Commands commands;
    if (!CreateCommands(dev, &commands)) { return 1; }
    const VkCommandBuffer cmd = commands.graphics.buffer;

    // ---- 동기화 오브젝트 ----
    //
    // 셋이 있는데 **개수의 근거가 서로 다르다:**
    //
    //   imageAvailable  GPU->GPU  frames-in-flight당 1  acquire를 부르기 **전에는**
    //                                                   어느 이미지인지 모른다
    //   renderFinished  GPU->GPU  **이미지당**          Swapchain 안에 있다
    //   inFlight        GPU->CPU  frames-in-flight당 1  CPU가 다음 프레임을 준비해도 되나
    //
    // 첫째와 둘째가 서로 반대인 것이 핵심이다. **이 비대칭이 "프레임에 묶인 자원"과
    // "스왑체인에 묶인 자원"을 가르는 선**이고, 클래스를 나눌 때 이 선을 따르게 된다.
    // 지금 frames-in-flight = 1이라 각각 하나씩이다.
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    if (dev.table.vkCreateSemaphore(dev.handle, &semaphoreInfo, nullptr, &imageAvailable)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateSemaphore(imageAvailable) failed\n");
        return 1;
    }

    // **신호된 상태로 만든다.** 첫 프레임엔 기다릴 이전 프레임이 없다.
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence inFlight = VK_NULL_HANDLE;
    if (dev.table.vkCreateFence(dev.handle, &fenceInfo, nullptr, &inFlight) != VK_SUCCESS) {
        LOG("[vk] vkCreateFence(inFlight) failed\n");
        return 1;
    }

    // ---- 루프 ----
    LOG("close the window to exit.\n");

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // 0. 그릴 곳 확보. 리사이즈 통보는 창 콜백이 window에 직접 세워놨다.
        if (!EnsureSwapchain(inst, dev, &window)) {
            continue;   // 최소화 중. 이번 프레임은 없다
        }
        Swapchain& swapchain = window.swapchain;

        // 1. 이전 프레임이 끝나기를 기다린다 (GPU -> CPU)
        dev.table.vkWaitForFences(dev.handle, 1, &inFlight, VK_TRUE, UINT64_MAX);

        // 2. 이미지를 하나 빌린다
        uint32_t imageIndex = 0;
        const VkResult acquired = dev.table.vkAcquireNextImageKHR(
            dev.handle, swapchain.handle, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex);

        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            window.swapchainOutOfDate = true;
            continue;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            LOG("[vk] vkAcquireNextImageKHR failed (%d)\n", acquired);
            break;
        }

        const SwapchainImage& target = swapchain.images[imageIndex];

        // 3. 펜스 리셋 (**acquire가 성공한 뒤에**)
        // 먼저 리셋하면, acquire가 실패해 제출 없이 돌아가는 프레임에서 펜스가 영영
        // 신호되지 않고 다음 WaitForFences가 영원히 걸린다.
        dev.table.vkResetFences(dev.handle, 1, &inFlight);

        // 4~11. 기록
        RecordFrame(dev.table, cmd, target, swapchain.extent);

        // 12. 제출
        // acquire가 끝나야 이미지에 쓸 수 있고(wait), 다 쓰면 present가 알아야 한다(signal).
        // 기다리는 지점을 COLOR_ATTACHMENT_OUTPUT으로 좁히면 그 앞 스테이지는 미리 돈다.
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = imageAvailable;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = target.renderFinished;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmdInfo.commandBuffer = cmd;

        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;

        if (dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &submit, inFlight) != VK_SUCCESS) {
            LOG("[vk] vkQueueSubmit2 failed\n");
            break;
        }

        // 13. 화면에 내보낸다
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &target.renderFinished;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain.handle;
        present.pImageIndices = &imageIndex;

        // SUBOPTIMAL은 에러가 아니다. 그려지긴 했고 다음 프레임에 다시 만들면 된다.
        const VkResult presented = dev.table.vkQueuePresentKHR(dev.queues.present, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            window.swapchainOutOfDate = true;
        }
    }

    // ---- 정리: 만든 역순 ----
    //
    // **여기가 통째로 사라지는 것이 클래스로 옮기는 진짜 이유다.** RAII로 가면
    // 이 열 줄이 "선언 순서"로 표현되고, 순서를 틀릴 방법 자체가 없어진다.
    dev.table.vkDeviceWaitIdle(dev.handle);   // GPU가 아직 작업 중일 수 있다

    dev.table.vkDestroyFence(dev.handle, inFlight, nullptr);
    dev.table.vkDestroySemaphore(dev.handle, imageAvailable, nullptr);
    DestroyCommands(dev, &commands);                  // 커맨드 버퍼도 같이 사라진다

    // 창에 묶인 셋(스왑체인 -> 서피스 -> 창)을 중첩 역순으로. **디바이스보다 먼저다** -
    // 스왑체인이 디바이스로 만들어졌기 때문이다.
    CloseWindow(inst, dev, &window);

    dev.table.vkDestroyDevice(dev.handle, nullptr);
    ShutdownWindowSystem();

    if (inst.messenger != VK_NULL_HANDLE) {
        inst.table.vkDestroyDebugUtilsMessengerEXT(inst.handle, inst.messenger, nullptr);
    }
    inst.table.vkDestroyInstance(inst.handle, nullptr);

    LOG("[vk] clean shutdown\n");
    return 0;
}
