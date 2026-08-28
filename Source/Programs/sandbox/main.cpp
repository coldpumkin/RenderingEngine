// 한 프레임을 화면에 띄우기까지, 위에서 아래로.
//
// **클래스가 없다. 의도적이다.**
//
// 한때 Vulkan 객체마다 RAII 래퍼 클래스가 있었다(VulkanInstance, VulkanDevice, ...).
// 그런데 그 래퍼들은 **전체 흐름을 한 번도 raw로 본 적 없는 상태에서** 만들어졌다.
// "Vulkan 객체 하나 = 클래스 하나"는 흐름을 보고 내린 결정이 아니라 흐름을 보기 전에
// 정한 규칙이었고, 그래서 클래스 경계마다 "이게 맞나"를 다시 묻게 됐다.
//
// 묶음의 근거는 흐름에서 나와야 한다. 무엇이 **같이 만들어지고, 같이 죽고, 같이 바뀌는가.**
// 그걸 보려면 먼저 펼쳐야 한다. 그게 이 파일이다.
//
// 옛 구조는 git master 브랜치에 그대로 있다.

#include <volk.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

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

#define LOG(...)  std::fprintf(stderr, __VA_ARGS__)

// ============================================================================
// 검증 레이어
// ============================================================================
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

// ============================================================================
// 스왑체인 - 창 크기가 바뀔 때마다 다시 만들어야 해서 함수로 뺐다
// ============================================================================

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

void DestroySwapchain(const VolkDeviceTable& vk, VkDevice device, Swapchain& sc) noexcept {
    if (sc.handle == VK_NULL_HANDLE) { return; }

    // GPU가 아직 이 이미지들을 쓰고 있을 수 있다. 스펙상 사용 중인 오브젝트 파괴는 금지다.
    vk.vkDeviceWaitIdle(device);

    for (SwapchainImage& img : sc.images) {
        vk.vkDestroySemaphore(device, img.renderFinished, nullptr);
        vk.vkDestroyImageView(device, img.view, nullptr);
        // img.image는 파괴하지 않는다 - vkGetSwapchainImagesKHR로 **조회**한 것이고
        // 스왑체인이 소유한다.
    }
    sc.images.clear();

    vk.vkDestroySwapchainKHR(device, sc.handle, nullptr);
    sc = Swapchain{};
}

// 실패 또는 "지금은 만들 수 없음"이면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// oldSwapchain을 넘겨야 하는 이유: **한 서피스에 스왑체인 둘이 동시에 존재할 수 없다.**
// 안 넘기면 재생성 자체가 실패한다(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR 등).
// 넘기면 그 순간 이전 것은 "은퇴" 상태가 되고, 우리가 따로 파괴해줘야 한다.
Swapchain CreateSwapchain(const VolkDeviceTable& vk,
                          VkPhysicalDevice physicalDevice,
                          VkDevice device,
                          VkSurfaceKHR surface,
                          VkSwapchainKHR oldSwapchain) noexcept {
    Swapchain sc;

    // ---- 서피스에게 "이 창은 무엇을 받나"를 묻는다 ----
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps) != VK_SUCCESS) {
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
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (formatCount == 0) {
        LOG("[vk] surface reports no formats\n");
        return sc;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());

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
    // 그래픽스와 프레젠트가 같은 큐 패밀리라서 EXCLUSIVE로 충분하다 (아래 큐 패밀리 선택 참고).
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = compositeAlpha;
    // FIFO는 **스펙이 항상 지원을 보장하는 유일한 모드**다. 수직동기와 같아 티어링이 없다.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    const VkResult created = vk.vkCreateSwapchainKHR(device, &info, nullptr, &sc.handle);
    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateSwapchainKHR failed (%d)\n", created);
        sc.handle = VK_NULL_HANDLE;
        return sc;
    }

    sc.format = surfaceFormat.format;
    sc.extent = caps.currentExtent;

    // ---- 이미지 조회 + 뷰/세마포어 생성 ----
    uint32_t actualCount = 0;
    vk.vkGetSwapchainImagesKHR(device, sc.handle, &actualCount, nullptr);
    std::vector<VkImage> rawImages(actualCount);
    vk.vkGetSwapchainImagesKHR(device, sc.handle, &actualCount, rawImages.data());

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
        vk.vkCreateImageView(device, &viewInfo, nullptr, &sc.images[i].view);

        // **이미지당 하나인 이유**: present가 이 이미지를 다 썼는지 CPU는 알 수 없다.
        // 프레임당 하나만 두면, 아직 present 중인 이미지의 세마포어를 다시 신호하게 된다.
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk.vkCreateSemaphore(device, &semInfo, nullptr, &sc.images[i].renderFinished);
    }

    LOG("[vk] swapchain %ux%u, %u images, format %d, FIFO\n",
        sc.extent.width, sc.extent.height, actualCount, static_cast<int>(sc.format));
    return sc;
}

// ============================================================================
// 이미지 레이아웃 전이 - 프레임에 두 번 나오는데 방향만 다르다
// ============================================================================
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

// ============================================================================
// 창 콜백 - 크기가 바뀌었다는 표시만 한다
// ============================================================================
//
// 크기 값을 안 받는 이유: 실제 크기는 서피스에게 물어야 정확하고(DPI 스케일링),
// 여기 오는 값과 다를 수 있다. 그리고 콜백은 아무 때나 오므로 그 자리에서 재생성하면
// GPU가 프레임 중일 수 있다. **표시만 하고 다음 프레임 시작에 처리한다.**
bool g_framebufferResized = false;

void OnFramebufferResized(GLFWwindow* /*window*/, int /*w*/, int /*h*/) {
    g_framebufferResized = true;
}

// ============================================================================
int main() {
    // ------------------------------------------------------------------------
    // 1. volk - Vulkan 로더 찾기
    // ------------------------------------------------------------------------
    // vulkan-1.dll을 런타임에 LoadLibrary로 찾는다. 없는 기계에서도 프로세스가 죽지 않고
    // 여기서 실패를 돌려받는다. (Vulkan::Vulkan을 정적 링크했다면 시작조차 못 했다.)
    if (volkInitialize() != VK_SUCCESS) {
        LOG("[vk] volkInitialize failed (vulkan-1.dll not found?)\n");
        return 1;
    }

    // vkEnumerateInstanceVersion은 Vulkan 1.1에서 추가됐다. 1.0 로더에서는 volk가 이
    // 포인터를 못 채우므로, **nullptr이라는 사실 자체가 "이 기계는 1.0"이라는 정보다.**
    if (vkEnumerateInstanceVersion == nullptr) {
        LOG("[vk] Vulkan 1.0 loader; need 1.3\n");
        return 1;
    }
    uint32_t loaderVersion = 0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < kRequiredApiVersion) {
        LOG("[vk] loader is %u.%u; need 1.3\n",
            VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return 1;
    }

    // ------------------------------------------------------------------------
    // 2. 인스턴스
    // ------------------------------------------------------------------------
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "LambdaEngine";
    appInfo.apiVersion = kRequiredApiVersion;

    // 서피스 확장은 선택이 아니다. 창에 그림을 못 내보내면 이 엔진은 할 일이 없다.
    // VK_KHR_surface는 플랫폼 공통, VK_KHR_win32_surface는 "HWND로 서피스를 만드는 법"이다.
    std::vector<const char*> instanceExtensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    std::vector<const char*> instanceLayers;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;

#if LAMBDA_ENABLE_VULKAN_VALIDATION
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeMessengerInfo();
    bool validationOn = false;
    if (HasInstanceLayer(kValidationLayer)) {
        instanceLayers.push_back(kValidationLayer);
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        // pNext에 걸면 **vkCreateInstance/vkDestroyInstance 자체의 문제도 잡힌다.**
        // 메신저를 따로 만들면 그 사이 구간이 감시 밖에 남는다.
        instanceInfo.pNext = &messengerInfo;
        validationOn = true;
    } else {
        LOG("[vk] validation layer not available (Vulkan SDK installed?)\n");
    }
#endif

    instanceInfo.enabledLayerCount = static_cast<uint32_t>(instanceLayers.size());
    instanceInfo.ppEnabledLayerNames = instanceLayers.data();
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        LOG("[vk] vkCreateInstance failed\n");
        return 1;
    }

    // **volkLoadInstance가 아니라 volkLoadInstanceOnly다.**
    // 전자는 디바이스 레벨 함수 포인터까지 전역에 채워버려서, 디바이스 테이블을 안 쓰고
    // 전역으로 불러도 조용히 동작한다. 멀티 디바이스에서 그건 틀린 디바이스를 부르는
    // 버그가 되고, 조용해서 안 잡힌다.
    volkLoadInstanceOnly(instance);

    LOG("[vk] instance created (Vulkan 1.3 requested, loader %u.%u)\n",
        VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));

    // ------------------------------------------------------------------------
    // 3. 디버그 메신저
    // ------------------------------------------------------------------------
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
#if LAMBDA_ENABLE_VULKAN_VALIDATION
    if (validationOn) {
        vkCreateDebugUtilsMessengerEXT(instance, &messengerInfo, nullptr, &messenger);
    }
#endif

    // ------------------------------------------------------------------------
    // 4. 창
    // ------------------------------------------------------------------------
    if (glfwInit() != GLFW_TRUE) {
        LOG("[glfw] glfwInit failed\n");
        return 1;
    }
    // GLFW는 기본적으로 OpenGL 컨텍스트를 같이 만든다. Vulkan을 쓰므로 끈다.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Lambda Engine", nullptr, nullptr);
    if (window == nullptr) {
        LOG("[glfw] glfwCreateWindow failed\n");
        return 1;
    }
    glfwSetFramebufferSizeCallback(window, OnFramebufferResized);

    // ------------------------------------------------------------------------
    // 5. 서피스 - "이 창"과 Vulkan을 잇는다
    // ------------------------------------------------------------------------
    // glfwCreateWindowSurface()도 있지만 쓰지 않는다. 직접 만들면 **창 라이브러리와
    // Vulkan이 서로를 모르는 상태로 남는다** - GLFW가 VkInstance를 알 필요가 없다.
    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = glfwGetWin32Window(window);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS) {
        LOG("[vk] vkCreateWin32SurfaceKHR failed\n");
        return 1;
    }

    // ------------------------------------------------------------------------
    // 6. 물리 디바이스 고르기 + 큐 패밀리 찾기
    // ------------------------------------------------------------------------
    //
    // **큐 패밀리가 뭔가**
    //
    // GPU는 명령을 "큐"에 넣어야 실행한다. 그런데 모든 큐가 모든 일을 하지는 않는다.
    // GPU는 큐를 **패밀리**로 묶어서 "이 그룹은 그래픽스+컴퓨트+전송을 할 수 있고,
    // 저 그룹은 전송만 전담한다"는 식으로 알려준다. 전송 전담 패밀리는 보통 DMA 엔진이라
    // 그래픽스와 **병렬로** 돌 수 있다 - 그게 패밀리를 나눠 놓은 이유다.
    //
    // 우리가 필요한 것 둘:
    //   VK_QUEUE_GRAPHICS_BIT  그리기 명령을 받을 수 있는가
    //   present 지원          그 결과를 화면에 내보낼 수 있는가
    //
    // **둘을 한 패밀리에서 찾는다.** 데스크톱 GPU에서는 사실상 항상 같은 패밀리가 둘 다
    // 한다. 나뉘는 경우를 지원하려면 큐를 둘 만들고 이미지 소유권을 큐 사이에서 넘겨야
    // 하는데(imageSharingMode = CONCURRENT 또는 명시적 ownership transfer),
    // 그건 실제로 나뉜 GPU를 만나면 그때 한다. 지금 만들면 **검증할 수 없는 코드**다.
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG("[vk] no Vulkan-capable GPU\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;
    VkPhysicalDeviceProperties chosenProps{};
    int bestScore = -1;

    for (VkPhysicalDevice candidate : gpus) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);

        // (a) API 버전
        if (props.apiVersion < kRequiredApiVersion) { continue; }

        // (b) 1.3 기능이 실제로 켜져 있는가 (버전을 지원해도 꺼져 있을 수 있다)
        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(candidate, &features2);
        if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE) {
            continue;
        }

        // (c) 스왑체인 확장
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, available.data());

        bool hasAllExtensions = true;
        for (const char* required : kRequiredDeviceExtensions) {
            bool found = false;
            for (const VkExtensionProperties& ext : available) {
                if (std::strcmp(ext.extensionName, required) == 0) { found = true; break; }
            }
            if (!found) { hasAllExtensions = false; break; }
        }
        if (!hasAllExtensions) { continue; }

        // (d) 그래픽스 + present를 둘 다 하는 큐 패밀리
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        uint32_t found = UINT32_MAX;
        for (uint32_t i = 0; i < familyCount; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) { continue; }

            // **서피스가 아니라 플랫폼에 묻는다.** 이 함수는 "이 큐 패밀리가 Win32
            // 데스크톱에 present 할 수 있는가"를 답하고, 창이 없어도 부를 수 있다.
            // VkBool32를 돌려주는 것에 주목 - 실패할 수 있는 조회가 아니라 성질이다.
            //
            // (이 함수는 Win32/Wayland/Xcb/Xlib에만 있다. Android/iOS/macOS엔 없어서
            //  언리얼은 "서피스가 생긴 뒤 확인"하는 2단계 초기화를 쓴다.)
            if (vkGetPhysicalDeviceWin32PresentationSupportKHR(candidate, i) == VK_TRUE) {
                found = i;
                break;
            }
        }
        if (found == UINT32_MAX) { continue; }

        // 자격을 통과한 것들 중 외장 GPU를 선호한다.
        const int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000 : 0;
        if (score > bestScore) {
            bestScore = score;
            physicalDevice = candidate;
            queueFamily = found;
            chosenProps = props;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        LOG("[vk] no GPU meets requirements (Vulkan 1.3 + dynamicRendering + sync2 + swapchain)\n");
        return 1;
    }

    // 고른 큐 패밀리가 **이 서피스**에도 present 되는지 확인한다.
    // 위의 Win32 확인은 "플랫폼에 대해"이고 이건 "이 창에 대해"다. 층이 다르고
    // 서로를 대신하지 않는다.
    VkBool32 surfaceSupported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamily, surface, &surfaceSupported);
    if (surfaceSupported != VK_TRUE) {
        LOG("[vk] chosen queue family cannot present to this surface\n");
        return 1;
    }

    LOG("[vk] GPU: %s (queue family %u)\n", chosenProps.deviceName, queueFamily);

    // ------------------------------------------------------------------------
    // 7. 논리 디바이스 + 큐 + 디바이스 함수 테이블
    // ------------------------------------------------------------------------
    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    // 지원 여부를 확인만 하는 게 아니라 **켜달라고 요청**해야 쓸 수 있다.
    VkPhysicalDeviceVulkan13Features enable13 = RequiredFeatures13();

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &enable13;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount =
        static_cast<uint32_t>(std::size(kRequiredDeviceExtensions));
    deviceInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        LOG("[vk] vkCreateDevice failed\n");
        return 1;
    }

    // **전역이 아니라 테이블로 받는다.** 전역(volkLoadDevice)은 마지막으로 로드한
    // 디바이스로 덮인다. 디바이스가 둘이 되는 순간 조용히 틀린 디바이스를 부르게 되고,
    // 조용해서 안 잡힌다. 지금 디바이스는 하나지만 **테이블을 쓰면 그 버그가 아예
    // 표현 불가능해진다.**
    VolkDeviceTable vk{};
    volkLoadDeviceTable(&vk, device);

    VkQueue queue = VK_NULL_HANDLE;
    vk.vkGetDeviceQueue(device, queueFamily, 0, &queue);

    // ------------------------------------------------------------------------
    // 8. 스왑체인
    // ------------------------------------------------------------------------
    Swapchain swapchain = CreateSwapchain(vk, physicalDevice, device, surface, VK_NULL_HANDLE);
    if (swapchain.handle == VK_NULL_HANDLE) {
        LOG("[vk] initial swapchain creation failed\n");
        return 1;
    }

    // ------------------------------------------------------------------------
    // 9. 커맨드 풀 + 커맨드 버퍼
    // ------------------------------------------------------------------------
    // 커맨드 풀은 **큐 패밀리에 묶인다.** 창도 프레임도 모른다.
    // RESET_COMMAND_BUFFER: 풀 전체가 아니라 버퍼 하나만 개별 리셋할 수 있게 한다.
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    if (vk.vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        LOG("[vk] vkCreateCommandPool failed\n");
        return 1;
    }

    // PRIMARY: 큐에 직접 제출할 수 있다. SECONDARY는 다른 버퍼 안에서만 실행된다.
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vk.vkAllocateCommandBuffers(device, &allocInfo, &cmd);
    // 버퍼는 따로 반납하지 않는다 - 풀을 파괴하면 같이 사라진다.

    // ------------------------------------------------------------------------
    // 10. 동기화 오브젝트
    // ------------------------------------------------------------------------
    //
    // 셋이 있는데 **개수의 근거가 서로 다르다:**
    //
    //   imageAvailable  GPU->GPU  frames-in-flight당 1  acquire를 부르기 **전에는**
    //                                                   어느 이미지인지 모른다
    //   renderFinished  GPU->GPU  **이미지당**          위 스왑체인 코드에 있다
    //   inFlight        GPU->CPU  frames-in-flight당 1  CPU가 다음 프레임을 준비해도 되나
    //
    // 첫째와 둘째가 서로 반대인 것이 핵심이다. 이 비대칭이 "프레임에 묶인 자원"과
    // "스왑체인에 묶인 자원"을 가르는 선이고, **나중에 클래스를 나눌 때 이 선을 따르게 된다.**
    //
    // 지금 frames-in-flight = 1이라 각각 하나씩이다.
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    vk.vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailable);

    // **신호된 상태로 만든다.** 첫 프레임엔 기다릴 이전 프레임이 없다.
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence inFlight = VK_NULL_HANDLE;
    vk.vkCreateFence(device, &fenceInfo, nullptr, &inFlight);

    // ------------------------------------------------------------------------
    // 11. 루프
    // ------------------------------------------------------------------------
    LOG("close the window to exit.\n");

    bool swapchainOutOfDate = false;

    while (glfwWindowShouldClose(window) == 0) {
        glfwPollEvents();

        if (g_framebufferResized) {
            g_framebufferResized = false;
            swapchainOutOfDate = true;
        }

        // ---- 0. 그릴 곳 확보 ----
        if (swapchainOutOfDate || swapchain.handle == VK_NULL_HANDLE) {
            vk.vkDeviceWaitIdle(device);

            const VkSwapchainKHR retiring = swapchain.handle;
            Swapchain fresh =
                CreateSwapchain(vk, physicalDevice, device, surface, retiring);

            // 이전 것을 파괴한다. oldSwapchain으로 넘겼어도 **파괴는 우리 몫이다** -
            // 넘긴 것은 "은퇴시켜라"는 뜻이지 "네가 지워라"가 아니다.
            DestroySwapchain(vk, device, swapchain);

            swapchain = std::move(fresh);
            swapchainOutOfDate = false;
        }

        if (swapchain.handle == VK_NULL_HANDLE) {
            continue;   // 최소화 중. 이번 프레임은 없다
        }

        // ---- 1. 이전 프레임이 끝나기를 기다린다 (GPU -> CPU) ----
        vk.vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX);

        // ---- 2. 이미지를 하나 빌린다 ----
        uint32_t imageIndex = 0;
        const VkResult acquired = vk.vkAcquireNextImageKHR(
            device, swapchain.handle, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex);

        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchainOutOfDate = true;
            continue;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            LOG("[vk] vkAcquireNextImageKHR failed (%d)\n", acquired);
            break;
        }

        const SwapchainImage& target = swapchain.images[imageIndex];

        // ---- 3. 펜스 리셋 (acquire가 성공한 뒤에) ----
        // 먼저 리셋하면, acquire가 실패해 제출 없이 돌아가는 프레임에서 펜스가 영영
        // 신호되지 않고 다음 WaitForFences가 영원히 걸린다.
        vk.vkResetFences(device, 1, &inFlight);

        // ---- 4~5. 기록 시작 ----
        vk.vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        // ONE_TIME_SUBMIT: 한 번 제출하고 버릴 기록이라고 드라이버에 알린다.
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(cmd, &beginInfo);

        // ---- 6. 그릴 수 있는 레이아웃으로 ----
        // oldLayout이 UNDEFINED인 것은 이전 내용을 안 쓰기 때문이다 - 어차피
        // loadOp=CLEAR로 덮는다. 보존을 요구하면 드라이버가 실제로 복사를 해야 한다.
        RecordLayoutTransition(vk, cmd, target.image,
                               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        // ---- 7. 렌더링 시작 ----
        // 다이나믹 렌더링: VkRenderPass/VkFramebuffer 객체를 미리 만들지 않는다.
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = target.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = swapchain.extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;

        vk.vkCmdBeginRendering(cmd, &rendering);

        // ---- 8. 드로우콜이 들어올 자리 ----

        // ---- 9. 렌더링 끝 ----
        vk.vkCmdEndRendering(cmd);

        // ---- 10. present 가능한 레이아웃으로 ----
        RecordLayoutTransition(vk, cmd, target.image,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        // ---- 11. 기록 끝 ----
        vk.vkEndCommandBuffer(cmd);

        // ---- 12. 제출 ----
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

        if (vk.vkQueueSubmit2(queue, 1, &submit, inFlight) != VK_SUCCESS) {
            LOG("[vk] vkQueueSubmit2 failed\n");
            break;
        }

        // ---- 13. 화면에 내보낸다 ----
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &target.renderFinished;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain.handle;
        present.pImageIndices = &imageIndex;

        // SUBOPTIMAL은 에러가 아니다. 그려지긴 했고 다음 프레임에 다시 만들면 된다.
        const VkResult presented = vk.vkQueuePresentKHR(queue, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            swapchainOutOfDate = true;
        }
    }

    // ------------------------------------------------------------------------
    // 12. 정리 - 만든 역순
    // ------------------------------------------------------------------------
    // GPU가 아직 작업 중일 수 있다. 스펙상 사용 중인 오브젝트 파괴는 금지다.
    vk.vkDeviceWaitIdle(device);

    vk.vkDestroyFence(device, inFlight, nullptr);
    vk.vkDestroySemaphore(device, imageAvailable, nullptr);
    vk.vkDestroyCommandPool(device, commandPool, nullptr);   // 커맨드 버퍼도 같이 사라진다
    DestroySwapchain(vk, device, swapchain);
    vk.vkDestroyDevice(device, nullptr);

    // 서피스는 인스턴스가 만들었으므로 인스턴스 레벨 함수로 파괴한다.
    // **창보다 먼저 죽어야 한다** - 죽은 HWND를 참조하게 된다.
    vkDestroySurfaceKHR(instance, surface, nullptr);

    glfwDestroyWindow(window);
    glfwTerminate();

#if LAMBDA_ENABLE_VULKAN_VALIDATION
    if (messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    }
#endif
    vkDestroyInstance(instance, nullptr);

    LOG("[vk] clean shutdown\n");
    return 0;
}
