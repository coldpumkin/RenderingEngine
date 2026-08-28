#include "NativeWrappers/VulkanInstance.h"

#include "RHILog.h"
#include "VulkanRequirements.h"
#include "VulkanResult.h"

#include <cstring>
#include <vector>

namespace LambdaEngine {
namespace {

#if LAMBDA_ENABLE_VULKAN_VALIDATION

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// 반환값은 "이 Vulkan 호출을 중단시킬까"를 뜻한다. 검증 레이어를 시험하는 용도가
// 아니면 항상 VK_FALSE여야 한다.
//
// 접두어가 [Vulkan]인 이유: 우리가 찍는 게 아니라 레이어가 알려주는 것이고,
// 우리 코드가 아닌 것(다른 프로그램이 설치한 암묵적 레이어)도 여기로 들어온다.
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) noexcept {

    const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR"
                      : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN"
                      : "INFO";

    std::fprintf(stderr, "[Vulkan %s] %s\n", level,
                 data->pMessage != nullptr ? data->pMessage : "(no message)");
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MakeMessengerInfo() noexcept {
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    // VERBOSE/INFO는 지금 단계에선 소음이 커서 뺀다.
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugCallback;
    return info;
}

// vkEnumerateInstanceLayerProperties는 volkInitialize()가 채우는 몇 안 되는 함수 중
// 하나다. 인스턴스가 없어도 부를 수 있어야 해서 부트스트랩에 포함돼 있고, 덕분에
// "무엇을 켤 수 있는지 먼저 조사한 다음 인스턴스를 만든다"가 가능하다.
bool HasInstanceLayer(const char* name) noexcept {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

#endif // LAMBDA_ENABLE_VULKAN_VALIDATION

} // namespace

std::unique_ptr<VulkanInstance> VulkanInstance::Create() noexcept {
    // 로더 탐색. vulkan-1.dll이 없는 기계에서도 실패를 반환할 뿐 죽지 않는다.
    // Vulkan::Vulkan을 정적 링크했다면 프로세스가 시작조차 못 했다.
    const VkResult loaderResult = volkInitialize();
    if (loaderResult != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("volkInitialize failed: %s (vulkan-1.dll not found?)",
                         ToString(loaderResult));
        return nullptr;
    }

    // vkEnumerateInstanceVersion은 Vulkan 1.1에서 추가된 함수라 1.0 로더에서는 volk가
    // 이 포인터를 채우지 못한다. 즉 nullptr이라는 사실 자체가 "이 기계는 1.0"이라는 정보다.
    //
    // 이 검사가 없어도 vkCreateInstance가 실패하지만, 그 메시지로는 무엇이 부족한지
    // 알 수 없다. 실패 지점이 원인을 정확히 안다는 원칙.
    if (vkEnumerateInstanceVersion == nullptr) {
        LAMBDA_LOG_ERROR("Vulkan 1.0 loader; this engine requires %u.%u",
                         VK_API_VERSION_MAJOR(kRequiredApiVersion),
                         VK_API_VERSION_MINOR(kRequiredApiVersion));
        return nullptr;
    }

    uint32_t loaderVersion = 0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < kRequiredApiVersion) {
        LAMBDA_LOG_ERROR("loader supports Vulkan %u.%u; this engine requires %u.%u",
                         VK_API_VERSION_MAJOR(loaderVersion),
                         VK_API_VERSION_MINOR(loaderVersion),
                         VK_API_VERSION_MAJOR(kRequiredApiVersion),
                         VK_API_VERSION_MINOR(kRequiredApiVersion));
        return nullptr;
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "LambdaEngine";
    appInfo.apiVersion = kRequiredApiVersion;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;

    std::vector<const char*> layers;

    // 서피스 확장은 선택이 아니다. 창에 그림을 못 내보내면 이 엔진은 할 일이 없다.
    // VK_KHR_surface는 플랫폼 공통, VK_KHR_win32_surface는 "HWND로 서피스를 만드는 법"이다.
    std::vector<const char*> extensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

#if LAMBDA_ENABLE_VULKAN_VALIDATION
    // 선언과 사용처가 전부 #if 안에 있어야 한다. 밖에 두면 검증을 끈 빌드에서
    // C4189가 난다. #if는 스코프를 만들지 않으므로 아래 블록에서도 보인다.
    bool validationEnabled = false;

    // 검증 레이어는 없을 수 있다(SDK 미설치). 개발 편의 기능이라 실패로 치지 않는다.
    if (HasInstanceLayer(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        validationEnabled = true;
    } else {
        LAMBDA_LOG_ERROR("%s not available - validation disabled", kValidationLayer);
    }

    // pNext 체이닝: vkCreateInstance/vkDestroyInstance 자체의 검증 메시지를 받는 방법.
    // 정식 메신저는 인스턴스가 있어야 만들 수 있어 그 구간이 사각지대가 된다.
    const VkDebugUtilsMessengerCreateInfoEXT bootstrapMessenger = MakeMessengerInfo();
    if (validationEnabled) {
        instanceInfo.pNext = &bootstrapMessenger;
    }
#endif

    instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkCreateInstance failed: %s", ToString(result));
        return nullptr;
    }

    // volkLoadInstance가 아니라 volkLoadInstanceOnly다. 전자는 전역 디바이스 함수까지
    // 채워서, table 없이 디바이스 함수를 불러도 조용히 동작한다. 후자는 전역이 nullptr로
    // 남아 즉시 크래시한다 - 규칙 위반을 시끄럽게 만드는 쪽을 고른다.
    //
    // 이 호출을 뺄 수는 없다. volkLoadDeviceTable이 전역 vkGetDeviceProcAddr에
    // 의존하기 때문이다.
    volkLoadInstanceOnly(instance);

    // 우리가 실제로 쓸 함수 테이블. 위 전역 로드와 별개로 따로 채운다.
    VolkInstanceTable table{};
    volkLoadInstanceTable(&table, instance);

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
#if LAMBDA_ENABLE_VULKAN_VALIDATION
    if (validationEnabled) {
        const VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeMessengerInfo();
        const VkResult messengerResult =
            table.vkCreateDebugUtilsMessengerEXT(instance, &messengerInfo, nullptr, &messenger);
        if (messengerResult != VK_SUCCESS) {
            // 인스턴스는 멀쩡하다. 검증만 못 받을 뿐이라 실패로 치지 않는다.
            LAMBDA_LOG_ERROR("vkCreateDebugUtilsMessengerEXT failed: %s",
                             ToString(messengerResult));
            messenger = VK_NULL_HANDLE;
        }
    }
#endif

    // requested는 우리가 쓰겠다고 선언한 값, loader는 이 기계의 최대치다.
    // 레이어 상태는 우리가 켠 것만 말한다 - 암묵적 레이어는 통제 대상이 아니다.
    LAMBDA_LOG_INFO("instance created (requested Vulkan %u.%u, loader %u.%u, validation %s)",
                    VK_API_VERSION_MAJOR(kRequiredApiVersion),
                    VK_API_VERSION_MINOR(kRequiredApiVersion),
                    VK_API_VERSION_MAJOR(loaderVersion),
                    VK_API_VERSION_MINOR(loaderVersion),
                    messenger != VK_NULL_HANDLE ? "on" : "off");

    return std::unique_ptr<VulkanInstance>(new VulkanInstance(instance, table, messenger));
}

VulkanInstance::VulkanInstance(VkInstance instance,
                               const VolkInstanceTable& table,
                               VkDebugUtilsMessengerEXT messenger) noexcept
    : instance_(instance), table_(table), messenger_(messenger) {}

VulkanInstance::~VulkanInstance() {
    // 메신저는 인스턴스에 붙어 있는 자식이라 먼저 없앤다.
    if (messenger_ != VK_NULL_HANDLE) {
        table_.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    }
    table_.vkDestroyInstance(instance_, nullptr);
    LAMBDA_LOG_INFO("instance destroyed");
}

} // namespace LambdaEngine
