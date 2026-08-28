#include "VulkanPhysicalDevice.h"

#include "RHILog.h"
#include "VulkanRequirements.h"
#include "VulkanResult.h"

#include <cstring>
#include <vector>

namespace LambdaEngine {
namespace {

constexpr uint32_t kInvalidQueueFamily = UINT32_MAX;

// 아래 헬퍼는 전부 **인스턴스 레벨 함수**를 부른다 (물리 디바이스 조회는 인스턴스 레벨이다).
// 그래서 테이블을 인자로 받는다. 전역을 쓰지 않는다는 결정(D2)이 여기까지 내려온 것이다.
//
// 래퍼가 아니라 테이블과 핸들을 그대로 받는 것은 D88의 예외다. D88이 막으려는 것은
// "핸들과 함수가 다른 객체에서 와서 섞이는 것"인데, 이 헬퍼들의 호출자는 같은 파일
// 몇 줄 아래에 있고 거기서 한 래퍼로부터 한 번에 꺼낸다. 섞일 경로가 없다.
// 게다가 후보 루프를 돌므로 매 후보마다 테이블을 다시 꺼내게 된다.

// 그래픽스 **그리고** 이 플랫폼으로의 프레젠트를 둘 다 지원하는 첫 큐 패밀리.
//
// 서피스가 아니라 플랫폼에 묻는다: vkGetPhysicalDeviceWin32PresentationSupportKHR은
// "이 큐 패밀리가 Win32 데스크톱에 present 할 수 있는가"를 답한다. 창이 없어도 물을 수
// 있으므로 **GPU 선택이 창에 묶이지 않는다.**
//
// 언리얼은 이렇게 못 한다 - 이 함수가 있는 플랫폼은 Win32/Wayland/Xcb/Xlib뿐이고
// Android·iOS·macOS엔 없어서, 공통 경로가 "서피스가 생긴 뒤 확인"밖에 없다
// (FVulkanDevice::SetupPresentQueue). 그래서 UE는 2단계 초기화가 됐고, 우리는
// Win32만 하므로(D42) 생성 시점에 확정할 수 있다.
//
// 둘을 한 패밀리에서 찾는 이유: 데스크톱 GPU에서는 사실상 항상 같은 패밀리가 둘 다 한다.
// 나뉘는 경우를 지원하려면 큐를 둘 만들고 이미지 소유권을 큐 사이에서 넘겨야 하는데,
// 그건 두 번째 사례가 생기면 그때 한다. **지금 나누면 검증할 수 없는 코드를 만드는 것이다.**
uint32_t FindGraphicsPresentQueueFamily(const VolkInstanceTable& it,
                                        VkPhysicalDevice physicalDevice) noexcept {
    uint32_t count = 0;
    it.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);

    std::vector<VkQueueFamilyProperties> families(count);
    it.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        // VkResult가 아니라 VkBool32를 돌려준다 - 실패할 수 있는 조회가 아니라 성질이다.
        if (it.vkGetPhysicalDeviceWin32PresentationSupportKHR(physicalDevice, i) == VK_TRUE) {
            return i;
        }
    }
    return kInvalidQueueFamily;
}

// 우리가 요구하는 1.3 기능을 지원하는가.
// 요구 목록은 VulkanRequirements.h 한 곳에서 나온다 - 확인과 활성화가 같은 값을 봐야 한다.
bool SupportsRequiredFeatures(const VolkInstanceTable& it,
                              VkPhysicalDevice physicalDevice) noexcept {
    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features13;
    it.vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    const VkPhysicalDeviceVulkan13Features required = RequiredVulkan13Features();
    return (required.dynamicRendering == VK_FALSE || features13.dynamicRendering == VK_TRUE)
        && (required.synchronization2 == VK_FALSE || features13.synchronization2 == VK_TRUE);
}

bool SupportsRequiredExtensions(const VolkInstanceTable& it,
                                VkPhysicalDevice physicalDevice) noexcept {
    uint32_t count = 0;
    if (it.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr)
        != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> available(count);
    if (it.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data())
        != VK_SUCCESS) {
        return false;
    }

    for (const char* required : kRequiredDeviceExtensions) {
        bool found = false;
        for (const VkExtensionProperties& extension : available) {
            if (std::strcmp(extension.extensionName, required) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// 선택 기준: 디스크리트 GPU 우선, 없으면 통합 GPU, 그다음 아무거나.
// 노트북에서 통합 GPU가 0번으로 먼저 열거되는 일이 흔해서 열거 순서를 믿으면 안 된다.
//
// **점수는 "얼마나 좋은가"만 본다. "쓸 수 있는가"는 후보 자격 검사가 따로 한다**.
// 둘을 섞으면 "점수 0이면 탈락" 같은 규칙이 생기는데, 그러면 왜 탈락했는지 알 수 없다.
int ScorePhysicalDevice(const VkPhysicalDeviceProperties& props) noexcept {
    switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 3;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
    default:                                     return 1;
    }
}

} // namespace

PhysicalDeviceSelection SelectPhysicalDevice(const VulkanInstance& instance) noexcept {
    const VolkInstanceTable& it = instance.Table();
    const VkInstance instanceHandle = instance.Handle();

    // Vulkan 열거 관례: 개수를 먼저 물어보고, 버퍼를 잡고, 다시 불러서 채운다.
    // "호출이 실패한 것"과 "성공했는데 디바이스가 0개인 것"은 원인이 완전히 다르므로
    // 따로 판단하고 따로 기록한다.
    uint32_t deviceCount = 0;
    const VkResult countResult =
        it.vkEnumeratePhysicalDevices(instanceHandle, &deviceCount, nullptr);
    if (countResult != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkEnumeratePhysicalDevices(count) failed: %s", ToString(countResult));
        return {};
    }
    if (deviceCount == 0) {
        LAMBDA_LOG_ERROR("no Vulkan-capable device found");
        return {};
    }

    std::vector<VkPhysicalDevice> candidates(deviceCount);
    const VkResult listResult =
        it.vkEnumeratePhysicalDevices(instanceHandle, &deviceCount, candidates.data());
    if (listResult == VK_INCOMPLETE) {
        // 두 호출 사이에 디바이스 목록이 늘어난 경우(eGPU 연결 등). 실패가 아니다.
        LAMBDA_LOG_ERROR("device list grew during enumeration; using %u", deviceCount);
    } else if (listResult != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkEnumeratePhysicalDevices(list) failed: %s", ToString(listResult));
        return {};
    }
    candidates.resize(deviceCount);

    PhysicalDeviceSelection best{};
    int bestScore = 0;

    for (VkPhysicalDevice candidate : candidates) {
        VkPhysicalDeviceProperties props{};
        it.vkGetPhysicalDeviceProperties(candidate, &props);

        // 자격 1 - API 버전. 이걸 안 보면 Vulkan 1.0 GPU도 통과해버리고,
        // vkCreateDevice까지 성공한 다음 vkCmdBeginRendering에서 죽는다.
        if (props.apiVersion < kRequiredApiVersion) {
            LAMBDA_LOG_ERROR("skip '%s': Vulkan %u.%u < required %u.%u",
                             props.deviceName,
                             VK_API_VERSION_MAJOR(props.apiVersion),
                             VK_API_VERSION_MINOR(props.apiVersion),
                             VK_API_VERSION_MAJOR(kRequiredApiVersion),
                             VK_API_VERSION_MINOR(kRequiredApiVersion));
            continue;
        }

        // 자격 2 - 1.3 기능들. 버전을 지원해도 기능이 꺼져 있을 수 있다.
        if (!SupportsRequiredFeatures(it, candidate)) {
            LAMBDA_LOG_ERROR("skip '%s': dynamicRendering or synchronization2 not supported",
                             props.deviceName);
            continue;
        }

        // 자격 3 - 스왑체인 확장. 없으면 화면에 아무것도 못 내보낸다.
        if (!SupportsRequiredExtensions(it, candidate)) {
            LAMBDA_LOG_ERROR("skip '%s': missing %s", props.deviceName,
                             VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            continue;
        }

        // 자격 4 - 그래픽스 + 이 플랫폼으로의 프레젠트를 둘 다 하는 큐 패밀리
        const uint32_t queueFamily = FindGraphicsPresentQueueFamily(it, candidate);
        if (queueFamily == kInvalidQueueFamily) {
            LAMBDA_LOG_ERROR("skip '%s': no queue family does graphics + present",
                             props.deviceName);
            continue;
        }

        const int score = ScorePhysicalDevice(props);
        if (score > bestScore) {
            bestScore = score;
            best.device = candidate;
            best.graphicsQueueFamily = queueFamily;
        }
    }

    if (best.device == VK_NULL_HANDLE) {
        LAMBDA_LOG_ERROR("found %u device(s) but none meets requirements",
                         static_cast<uint32_t>(candidates.size()));
    }
    return best;
}

} // namespace LambdaEngine
