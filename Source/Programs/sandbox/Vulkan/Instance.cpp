#include "Vulkan/Instance.h"

#include <cstring>
#include <vector>

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

bool CreateInstance(VulkanInstance* out) noexcept {
    VulkanInstance& inst = *out;

    // volk: vulkan-1.dll을 런타임에 LoadLibrary로 찾는다. 없는 기계에서도 프로세스가
    // 죽지 않고 여기서 실패를 돌려받는다. (Vulkan::Vulkan을 정적 링크했다면 시작조차 못 했다.)
    if (volkInitialize() != VK_SUCCESS) {
        LOG("[vk] volkInitialize failed (vulkan-1.dll not found?)\n");
        return false;
    }

    // vkEnumerateInstanceVersion은 Vulkan 1.1에서 추가됐다. 1.0 로더에서는 volk가 이
    // 포인터를 못 채우므로, **nullptr이라는 사실 자체가 "이 기계는 1.0"이라는 정보다.**
    if (vkEnumerateInstanceVersion == nullptr) {
        LOG("[vk] Vulkan 1.0 loader; need 1.3\n");
        return false;
    }
    uint32_t loaderVersion = 0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < kRequiredApiVersion) {
        LOG("[vk] loader is %u.%u; need 1.3\n",
            VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return false;
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
        return false;
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
        // **실패해도 인스턴스 생성은 성공이다** - 검증 메시지를 못 받을 뿐 렌더링은 된다.
        // 하지만 조용히 넘어가면 안 된다: 검증이 안 도는 줄 모르고 작업하게 되고,
        // 이 프로젝트에서 검증 레이어는 있으면 좋은 게 아니라 주요 안전망이다.
        if (inst.table.vkCreateDebugUtilsMessengerEXT(
                inst.handle, &messengerInfo, nullptr, &inst.messenger) != VK_SUCCESS) {
            LOG("[vk] 경고: 디버그 메신저를 만들지 못했다 - **검증 메시지가 안 나온다**\n");
            inst.messenger = VK_NULL_HANDLE;
        }
    }
#endif

    LOG("[vk] instance created (Vulkan 1.3 requested, loader %u.%u)\n",
        VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
    return true;
}

// **자기 힘으로 파괴한다.** 테이블도 핸들도 자기가 들고 있다.
VulkanInstance::~VulkanInstance() {
    if (handle == VK_NULL_HANDLE) { return; }   // 비어 있는 상태도 합법이다
    if (messenger != VK_NULL_HANDLE) {
        table.vkDestroyDebugUtilsMessengerEXT(handle, messenger, nullptr);
    }
    table.vkDestroyInstance(handle, nullptr);
}