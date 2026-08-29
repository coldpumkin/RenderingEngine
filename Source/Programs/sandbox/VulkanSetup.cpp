// 만들고 부수는 곳 - **초기화 경로**.
//
// 여기 있는 코드는 프로그램당 한 번(또는 리사이즈마다) 돈다. 힙 할당도 로그도 자유롭다.
// 매 프레임 도는 코드는 main.cpp에 있고, 거기는 규칙이 다르다.
//
// 무엇이 있는지는 Vulkan.h를 보면 된다. 여기는 "어떻게"만 있다.

#include "Vulkan.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <cstring>
#include <string>
#include <utility>

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

    // 메모리 타입 목록을 여기서 한 번 물어 담는다 (인스턴스 레벨 조회다).
    inst.table.vkGetPhysicalDeviceMemoryProperties(dev.gpu, &dev.memoryProperties);

    return dev;
}

// ============================================================================
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
VkCommandPool CreateCommandPool(const VulkanDevice& dev, uint32_t queueFamily) noexcept {
    // RESET_COMMAND_BUFFER: 풀 전체가 아니라 버퍼 하나만 개별 리셋할 수 있게 한다.
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = queueFamily;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (dev.table.vkCreateCommandPool(dev.handle, &info, nullptr, &pool) != VK_SUCCESS) {
        LOG("[vk] vkCreateCommandPool failed (family %u)\n", queueFamily);
        return VK_NULL_HANDLE;
    }
    return pool;
}

bool CreateCommands(const VulkanDevice& dev, Commands* out) noexcept {
    *out = Commands{};

    out->graphics = CreateCommandPool(dev, dev.families.graphics);
    if (out->graphics == VK_NULL_HANDLE) { return false; }

    if (dev.families.HasCompute()) {
        out->compute = CreateCommandPool(dev, dev.families.compute);
        if (out->compute == VK_NULL_HANDLE) { return false; }
    }
    if (dev.families.HasTransfer()) {
        out->transfer = CreateCommandPool(dev, dev.families.transfer);
        if (out->transfer == VK_NULL_HANDLE) { return false; }
    }
    return true;
}

void DestroyCommands(const VulkanDevice& dev, Commands* c) noexcept {
    // 풀을 파괴하면 거기서 나온 커맨드 버퍼도 같이 사라진다.
    for (VkCommandPool pool : {c->graphics, c->compute, c->transfer}) {
        if (pool != VK_NULL_HANDLE) {
            dev.table.vkDestroyCommandPool(dev.handle, pool, nullptr);
        }
    }
    *c = Commands{};
}

// ---------------------------------------------------------------------------
// 프레임 자원 - frames-in-flight마다 한 벌
// ---------------------------------------------------------------------------
//
// **셋이 같은 신호 하나에 묶인다.** 판별은 이렇다:
// *"이 자원을 다시 써도 된다는 걸 무엇이 알려주는가?"*
//
//   cmd             pending 상태면 리셋할 수 없다      -> inFlight 펜스가 알려준다
//   imageAvailable  이전 wait(submit)이 끝나야 재signal -> inFlight 펜스가 알려준다
//   inFlight        그 자신이 신호다
//
// 셋 다 답이 같은 펜스 하나다. 그래서 개수도 같고(= frames-in-flight) 한 벌이다.
//
// **renderFinished가 여기 없는 이유도 같은 기준이다.** 그건 present가 기다리는데,
// present에는 완료를 알려주는 것이 없다(vkQueuePresentKHR은 펜스를 주지 않는다).
// 유일한 단서가 "acquire가 그 이미지를 다시 줬다"이고 그건 이미지 인덱스로만 오므로,
// 개수가 이미지 수가 되어 Swapchain 안에 산다.
//
// 지금 frames-in-flight = 1이라 한 벌이다. 2로 올리면 이 struct가 배열이 되고,
// 루프는 frames[frameIndex]를 돌려쓰게 된다.
bool CreateFrame(const VulkanDevice& dev, const Commands& commands, Frame* out) noexcept {
    *out = Frame{};

    // PRIMARY: 큐에 직접 제출할 수 있다. SECONDARY는 다른 버퍼 안에서만 실행된다.
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commands.graphics;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (dev.table.vkAllocateCommandBuffers(dev.handle, &allocInfo, &out->cmd) != VK_SUCCESS) {
        LOG("[vk] vkAllocateCommandBuffers failed\n");
        return false;
    }

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (dev.table.vkCreateSemaphore(dev.handle, &semaphoreInfo, nullptr, &out->imageAvailable)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateSemaphore(imageAvailable) failed\n");
        return false;
    }

    // **신호된 상태로 만든다.** 첫 프레임엔 기다릴 이전 프레임이 없다.
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dev.table.vkCreateFence(dev.handle, &fenceInfo, nullptr, &out->inFlight) != VK_SUCCESS) {
        LOG("[vk] vkCreateFence(inFlight) failed\n");
        return false;
    }
    return true;
}

void DestroyFrame(const VulkanDevice& dev, Frame* frame) noexcept {
    if (frame->inFlight != VK_NULL_HANDLE) {
        dev.table.vkDestroyFence(dev.handle, frame->inFlight, nullptr);
    }
    if (frame->imageAvailable != VK_NULL_HANDLE) {
        dev.table.vkDestroySemaphore(dev.handle, frame->imageAvailable, nullptr);
    }
    // cmd는 따로 반납하지 않는다 - 풀이 파괴될 때 같이 사라진다.
    *frame = Frame{};
}

// ============================================================================
// 7. 그래픽스 파이프라인 - **무엇으로 그리는가**
// ============================================================================
//
// 지금까지는 "어디에 그리는가"(스왑체인 이미지)만 있었다. 파이프라인은 그 반대편이다:
// 정점을 어떻게 화면 좌표로 바꾸고, 픽셀 색을 어떻게 정하는가.
//
// **파이프라인은 스왑체인 포맷에 묶인다.** 아래 pipelineRendering.pColorAttachmentFormats가
// 그 자리다 - 다이나믹 렌더링에서는 VkRenderPass 대신 여기에 포맷을 미리 적어둔다.
// 그래서 포맷이 바뀌면 파이프라인도 다시 만들어야 한다. **리사이즈는 포맷을 안 바꾸므로
// 지금은 재생성이 필요 없다** (크기는 동적 상태로 매 프레임 준다).

// SPIR-V 파일 하나를 읽어 VkShaderModule로.
//
// 실행 파일 옆 Shaders/에서 찾는다. CMake가 빌드할 때 거기로 떨군다.
VkShaderModule LoadShader(const VulkanDevice& dev, const char* path) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        LOG("[vk] cannot open shader: %s\n", path);
        return VK_NULL_HANDLE;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    // SPIR-V는 32비트 워드 배열이다. 크기가 4의 배수가 아니면 파일이 깨진 것이다.
    if (size <= 0 || (size % 4) != 0) {
        LOG("[vk] bad SPIR-V size %ld: %s\n", size, path);
        std::fclose(file);
        return VK_NULL_HANDLE;
    }

    // uint32_t 벡터로 읽는 이유: pCode가 const uint32_t*이고 **4바이트 정렬을 요구한다.**
    // char 배열로 읽어 캐스팅하면 정렬이 보장되지 않는다.
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    const size_t read = std::fread(code.data(), 1, static_cast<size_t>(size), file);
    std::fclose(file);
    if (read != static_cast<size_t>(size)) {
        LOG("[vk] short read: %s\n", path);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = static_cast<size_t>(size);   // **바이트 수**다 (워드 수가 아니다)
    info.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (dev.table.vkCreateShaderModule(dev.handle, &info, nullptr, &module) != VK_SUCCESS) {
        LOG("[vk] vkCreateShaderModule failed: %s\n", path);
        return VK_NULL_HANDLE;
    }
    return module;
}

// 파이프라인 + 레이아웃. 둘이 같이 태어나고 같이 죽는다.
Pipeline CreateTrianglePipeline(const VulkanDevice& dev, VkFormat colorFormat) noexcept {
    Pipeline pipeline;

    VkShaderModule vs = LoadShader(dev, "Shaders/triangle.vert.spv");
    VkShaderModule fs = LoadShader(dev, "Shaders/triangle.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        if (vs != VK_NULL_HANDLE) { dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr); }
        if (fs != VK_NULL_HANDLE) { dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr); }
        return pipeline;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";           // 진입점 함수 이름
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // ---- 정점 입력: GPU에게 "정점 데이터를 어떻게 읽어라"를 알려준다 ----
    //
    // binding  버퍼 슬롯 하나. stride는 한 정점의 크기, inputRate는 정점마다 넘길지
    //          인스턴스마다 넘길지. 여러 버퍼로 나눠 담으면 binding이 늘어난다.
    // attribute 그 안의 필드 하나. location은 셰이더의 layout(location=N) in과 짝이다.
    //
    // **format이 크기까지 정한다**: R32G32_SFLOAT = float 2개. 셰이더가 vec3으로 받아도
    // 여기가 vec2면 z는 0이 된다 - 조용히 틀리는 자리라 offsetof로 묶어둔다.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[2]{};
    attributes[0].location = 0;                             // layout(location = 0) in vec2
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);
    attributes[1].location = 1;                             // layout(location = 1) in vec3
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(std::size(attributes));
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // 정점 3개 = 삼각형 1개

    // **뷰포트와 시저를 동적 상태로 둔다.** 여기 값을 박으면 창 크기가 바뀔 때마다
    // 파이프라인을 다시 만들어야 한다. 동적으로 두면 매 프레임 vkCmdSetViewport로 준다.
    //
    // (여기서 말하는 "뷰포트"는 창이 아니라 **렌더 타겟 안의 사각 영역 + 깊이 범위**다.
    //  같은 단어의 다른 뜻이다.)
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    constexpr VkDynamicState kDynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(std::size(kDynamicStates));
    dynamicState.pDynamicStates = kDynamicStates;

    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;   // 뒷면도 그린다. 삼각형 하나뿐이라 상관없다
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;               // **0이면 검증 레이어가 잡는다**

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;   // MSAA 없음

    // 블렌딩 없음. 그린 색으로 그대로 덮는다.
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                   | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    // 레이아웃: 셰이더가 받는 외부 자원(유니폼, 푸시 상수)의 모양.
    // **지금은 비어 있다** - 셰이더가 아무것도 안 받는다. 그래도 만들어야 한다.
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (dev.table.vkCreatePipelineLayout(dev.handle, &layoutInfo, nullptr, &pipeline.layout)
            != VK_SUCCESS) {
        LOG("[vk] vkCreatePipelineLayout failed\n");
        dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr);
        dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr);
        return pipeline;
    }

    // **다이나믹 렌더링**: VkRenderPass 객체를 안 만드는 대신, 그릴 대상의 포맷을
    // 여기에 미리 알려준다. 파이프라인이 스왑체인 포맷에 묶이는 지점이 정확히 여기다.
    VkPipelineRenderingCreateInfo pipelineRendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pipelineRendering.colorAttachmentCount = 1;
    pipelineRendering.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &pipelineRendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamicState;
    info.layout = pipeline.layout;
    info.renderPass = VK_NULL_HANDLE;   // 다이나믹 렌더링이라 없다

    const VkResult created = dev.table.vkCreateGraphicsPipelines(
        dev.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.handle);

    // **셰이더 모듈은 파이프라인이 만들어지고 나면 필요 없다.** 코드가 파이프라인 안으로
    // 컴파일돼 들어갔다. 계속 들고 있을 이유가 없다.
    dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr);
    dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr);

    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateGraphicsPipelines failed (%d)\n", created);
        dev.table.vkDestroyPipelineLayout(dev.handle, pipeline.layout, nullptr);
        return Pipeline{};
    }

    LOG("[vk] triangle pipeline ready\n");
    return pipeline;
}

void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept {
    if (pipeline->handle != VK_NULL_HANDLE) {
        dev.table.vkDestroyPipeline(dev.handle, pipeline->handle, nullptr);
    }
    if (pipeline->layout != VK_NULL_HANDLE) {
        dev.table.vkDestroyPipelineLayout(dev.handle, pipeline->layout, nullptr);
    }
    *pipeline = Pipeline{};
}

// ============================================================================
// ============================================================================
// 9. 버퍼
// ============================================================================

uint32_t FindMemoryType(const VulkanDevice& dev,
                        uint32_t typeBits,
                        VkMemoryPropertyFlags required) noexcept {
    // GPU가 가진 메모리 타입 목록. **하드웨어마다 완전히 다르다.**
    //   외장 GPU: DEVICE_LOCAL(VRAM), HOST_VISIBLE(시스템 RAM),
    //             둘 다인 것(리사이저블 BAR 구간, 보통 256MB 또는 전체)
    //   내장 GPU: 대부분 DEVICE_LOCAL | HOST_VISIBLE (메모리를 CPU와 공유하므로)
    const VkPhysicalDeviceMemoryProperties& memory = dev.memoryProperties;

    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        // (a) 이 버퍼가 i번 타입에 놓일 수 있나 - **버퍼가 정한 제약**
        const bool allowedByBuffer = (typeBits & (1u << i)) != 0;

        // (b) i번 타입이 우리가 원하는 성질을 **전부** 가졌나 - **우리가 정한 요구**
        const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        const bool hasRequired = (flags & required) == required;

        if (allowedByBuffer && hasRequired) {
            return i;
        }
    }

    LOG("[vk] no memory type for typeBits=0x%x required=0x%x\n", typeBits, required);
    return UINT32_MAX;
}

Buffer CreateBuffer(const VulkanDevice& dev,
                    VkDeviceSize size,
                    VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags memoryProperties) noexcept {
    Buffer buffer;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    // EXCLUSIVE: 한 번에 한 큐 패밀리만 소유한다. 다른 패밀리가 쓰려면 소유권을
    // 명시적으로 넘겨야 한다. 지금은 그래픽스 큐만 만지므로 넘길 일이 없다.
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (dev.table.vkCreateBuffer(dev.handle, &info, nullptr, &buffer.handle) != VK_SUCCESS) {
        LOG("[vk] vkCreateBuffer failed\n");
        return Buffer{};
    }

    // **버퍼를 만들었다고 메모리가 붙은 게 아니다.** 여기서 요구사항을 물어본다:
    //   size          실제 필요한 크기 (정렬 때문에 요청보다 클 수 있다)
    //   alignment     시작 주소 정렬
    //   memoryTypeBits 놓을 수 있는 타입들
    VkMemoryRequirements requirements{};
    dev.table.vkGetBufferMemoryRequirements(dev.handle, buffer.handle, &requirements);

    const uint32_t typeIndex =
        FindMemoryType(dev, requirements.memoryTypeBits, memoryProperties);
    if (typeIndex == UINT32_MAX) {
        dev.table.vkDestroyBuffer(dev.handle, buffer.handle, nullptr);
        return Buffer{};
    }

    // **실제 할당.** 여기가 VMA가 대신하게 될 자리다 - VMA는 큰 덩어리를 미리 잡아두고
    // 그 안에서 잘라 쓴다. vkAllocateMemory는 호출 횟수에 상한이 있어서
    // (maxMemoryAllocationCount, 보통 4096) 버퍼마다 부르면 금방 바닥난다.
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = typeIndex;

    if (dev.table.vkAllocateMemory(dev.handle, &allocInfo, nullptr, &buffer.memory)
            != VK_SUCCESS) {
        LOG("[vk] vkAllocateMemory failed (%llu bytes)\n",
            static_cast<unsigned long long>(requirements.size));
        dev.table.vkDestroyBuffer(dev.handle, buffer.handle, nullptr);
        return Buffer{};
    }

    // 버퍼와 메모리를 잇는다. 이제야 쓸 수 있다.
    if (dev.table.vkBindBufferMemory(dev.handle, buffer.handle, buffer.memory, 0)
            != VK_SUCCESS) {
        LOG("[vk] vkBindBufferMemory failed\n");
        dev.table.vkFreeMemory(dev.handle, buffer.memory, nullptr);
        dev.table.vkDestroyBuffer(dev.handle, buffer.handle, nullptr);
        return Buffer{};
    }

    buffer.size = size;
    return buffer;
}

void DestroyBuffer(const VulkanDevice& dev, Buffer* buffer) noexcept {
    // **순서가 있다**: 버퍼를 먼저 부수고 메모리를 반납한다.
    // 메모리를 먼저 반납하면 버퍼가 없는 메모리를 가리키게 된다.
    if (buffer->handle != VK_NULL_HANDLE) {
        dev.table.vkDestroyBuffer(dev.handle, buffer->handle, nullptr);
    }
    if (buffer->memory != VK_NULL_HANDLE) {
        dev.table.vkFreeMemory(dev.handle, buffer->memory, nullptr);
    }
    *buffer = Buffer{};
}

Buffer CreateVertexBuffer(const VulkanDevice& dev,
                          const Commands& commands,
                          const void* data,
                          VkDeviceSize size) noexcept {
    // ---- 1. 스테이징: CPU가 쓸 수 있는 임시 버퍼 ----
    //
    // HOST_VISIBLE   vkMapMemory로 CPU 주소를 얻을 수 있다
    // HOST_COHERENT  CPU가 쓴 것이 GPU에게 자동으로 보인다.
    //                없으면 vkFlushMappedMemoryRanges를 직접 불러야 한다
    Buffer staging = CreateBuffer(dev, size,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (staging.handle == VK_NULL_HANDLE) { return Buffer{}; }

    void* mapped = nullptr;
    if (dev.table.vkMapMemory(dev.handle, staging.memory, 0, size, 0, &mapped) != VK_SUCCESS) {
        LOG("[vk] vkMapMemory failed\n");
        DestroyBuffer(dev, &staging);
        return Buffer{};
    }
    std::memcpy(mapped, data, static_cast<size_t>(size));
    dev.table.vkUnmapMemory(dev.handle, staging.memory);

    // ---- 2. 목적지: GPU 전용 메모리 ----
    //
    // DEVICE_LOCAL은 GPU가 가장 빠르게 읽는 메모리다. 대신 CPU가 매핑할 수 없는 것이
    // 보통이라(외장 GPU의 VRAM) 스테이징을 거쳐야 한다.
    // TRANSFER_DST: 복사의 목적지가 될 수 있다고 미리 알려준다.
    Buffer vertexBuffer = CreateBuffer(dev, size,
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                           | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vertexBuffer.handle == VK_NULL_HANDLE) {
        DestroyBuffer(dev, &staging);
        return Buffer{};
    }

    // ---- 3. GPU에게 복사를 시킨다 ----
    //
    // **그래픽스 큐를 쓴다. 전송 큐가 아니다.**
    // 전송 큐의 값어치는 그리는 동안 **동시에** 올리는 것인데, 이건 루프가 시작하기 전
    // 한 번뿐이라 겹칠 대상이 없다. 반면 큐를 바꾸면 **큐 패밀리 소유권 이전**
    // (release/acquire 배리어 한 쌍)이 필요해진다 - 얻는 것 없이 비용만 낸다.
    //
    // 전송 큐는 **그리는 중에 올려야 할 때** 값을 한다. 그때 옮긴다.
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commands.graphics;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (dev.table.vkAllocateCommandBuffers(dev.handle, &allocInfo, &cmd) != VK_SUCCESS) {
        LOG("[vk] vkAllocateCommandBuffers(upload) failed\n");
        DestroyBuffer(dev, &vertexBuffer);
        DestroyBuffer(dev, &staging);
        return Buffer{};
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev.table.vkBeginCommandBuffer(cmd, &beginInfo);

    VkBufferCopy region{};
    region.size = size;
    dev.table.vkCmdCopyBuffer(cmd, staging.handle, vertexBuffer.handle, 1, &region);

    dev.table.vkEndCommandBuffer(cmd);

    // 복사가 끝날 때까지 기다린다. **초기화 경로라 기다려도 된다** -
    // 매 프레임이면 펜스로 넘겨받아야 하지만 여기는 한 번뿐이다.
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;

    dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &submit, VK_NULL_HANDLE);
    dev.table.vkQueueWaitIdle(dev.queues.graphics);

    // ---- 4. 뒷정리 ----
    dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &cmd);
    DestroyBuffer(dev, &staging);

    LOG("[vk] vertex buffer ready (%llu bytes, device-local)\n",
        static_cast<unsigned long long>(size));
    return vertexBuffer;
}
