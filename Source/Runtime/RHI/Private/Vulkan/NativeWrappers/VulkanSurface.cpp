#include "NativeWrappers/VulkanSurface.h"

#include "RHILog.h"
#include "VulkanResult.h"

namespace LambdaEngine {

std::unique_ptr<VulkanSurface> VulkanSurface::Create(const VulkanInstance& instance,
                                                     NativeWindowHandle window) noexcept {
    if (window.instance == nullptr || window.window == nullptr) {
        // 프로그래머 오류다. 창을 만들지도 않고 RHI를 요청한 것이므로
        // 환경 문제(nullptr 반환)와 구분해서 로그를 남긴다 (D16).
        LAMBDA_LOG_ERROR("NativeWindowHandle is empty - window must be created first");
        return nullptr;
    }

    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = static_cast<HINSTANCE>(window.instance);
    info.hwnd = static_cast<HWND>(window.window);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result =
        instance.Table().vkCreateWin32SurfaceKHR(instance.Handle(), &info, nullptr, &surface);
    if (result != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkCreateWin32SurfaceKHR failed: %s", ToString(result));
        return nullptr;
    }

    LAMBDA_LOG_INFO("surface created");
    return std::unique_ptr<VulkanSurface>(new VulkanSurface(instance, surface));
}

VulkanSurface::VulkanSurface(const VulkanInstance& instance, VkSurfaceKHR surface) noexcept
    : instance_(instance), surface_(surface) {}

bool VulkanSurface::SupportsPresent(VkPhysicalDevice physicalDevice,
                                    uint32_t queueFamily) const noexcept {
    VkBool32 supported = VK_FALSE;
    const VkResult result = instance_.Table().vkGetPhysicalDeviceSurfaceSupportKHR(
        physicalDevice, queueFamily, surface_, &supported);
    if (result != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkGetPhysicalDeviceSurfaceSupportKHR failed: %s", ToString(result));
        return false;
    }
    return supported == VK_TRUE;
}

SurfaceSupport VulkanSurface::QuerySupport(VkPhysicalDevice physicalDevice) const noexcept {
    // 서피스 조회는 전부 **인스턴스 레벨 함수**다. 테이블은 우리가 이미 들고 있는
    // 인스턴스에서 나온다 - 밖으로 흘려보낼 상태가 없다.
    const VolkInstanceTable& it = instance_.Table();

    SurfaceSupport support{};

    const VkResult capsResult =
        it.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface_,
                                                     &support.capabilities);
    if (capsResult != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %s",
                         ToString(capsResult));
        return support;
    }

    // Vulkan 열거 관례: 개수를 먼저 묻고, 버퍼를 잡고, 다시 불러서 채운다.
    uint32_t formatCount = 0;
    if (it.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface_, &formatCount, nullptr)
            != VK_SUCCESS
        || formatCount == 0) {
        LAMBDA_LOG_ERROR("surface reports no formats");
        return support;
    }

    support.formats.resize(formatCount);
    if (it.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface_, &formatCount,
                                                support.formats.data()) != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkGetPhysicalDeviceSurfaceFormatsKHR(list) failed");
        return support;
    }

    support.valid = true;
    return support;
}

VulkanSurface::~VulkanSurface() {
    // 서피스는 인스턴스 레벨 함수로 파괴한다 (디바이스가 아니라).
    // 핸들과 함수가 같은 객체(instance_)에서 나오므로 섞일 수 없다.
    instance_.Table().vkDestroySurfaceKHR(instance_.Handle(), surface_, nullptr);
    LAMBDA_LOG_INFO("surface destroyed");
}

} // namespace LambdaEngine
