#pragma once

#include <volk.h>

namespace LambdaEngine {

// 이 엔진이 GPU에게 요구하는 것.
//
// 한 곳에 모으는 이유: 요구사항을 두 곳에서 본다. 선택기는 "지원하는가"를 확인하고,
// VulkanDevice는 "켜달라"고 요청한다. 어긋나면 디바이스는 만들어지고 드로우에서 죽는다.

// 다이나믹 렌더링이 1.3 코어라서 1.3이다. 임의로 고른 값이 아니다.
constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;

// 화면에 그리려면 반드시 있어야 하는 디바이스 확장.
// (VK_KHR_surface는 인스턴스 확장이다. 층이 다르다.)
constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// 버전을 지원해도 기능이 꺼져 있을 수 있어서 따로 확인한다.
// 이 함수 하나가 "확인"과 "요청"에 같은 값을 쓰게 만든다.
inline VkPhysicalDeviceVulkan13Features RequiredVulkan13Features() noexcept {
    VkPhysicalDeviceVulkan13Features features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.dynamicRendering = VK_TRUE;   // VkRenderPass/VkFramebuffer 없이 그린다
    features.synchronization2 = VK_TRUE;   // 배리어/제출 API 개정판
    return features;
}

} // namespace LambdaEngine
