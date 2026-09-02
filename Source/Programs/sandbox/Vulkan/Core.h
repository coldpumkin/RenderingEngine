#pragma once

// 모든 Vulkan 파일이 공유하는 것 - log, 요구사항, RAII 규약.
//
// Vulkan 호출은 예외 없이 두 층으로 갈린다:
//   Instance level  VkInstance 또는 VkPhysicalDevice로 dispatch
//   Device level    VkDevice · VkQueue · VkCommandBuffer로 dispatch
//
// API 자체의 선이고 파일이 이 선을 따라 나뉜다. 각 층이 자기 function table
// (VolkInstanceTable / VolkDeviceTable)을 들고, handle과 table이 한 객체에서
// 나와야 섞일 수 없다.
//
// 전역으로 남는 것은 bootstrap 4개뿐 - volkInitialize, vkEnumerateInstanceVersion,
// vkEnumerateInstanceLayerProperties, vkCreateInstance. Instance가 없을 때
// 부르는 함수라 table에 담을 수가 없다.
//
// RAII 규약 (자원 타입 전부에 적용):
//   1. 기본 생성 = 비어 있음. 그 상태가 합법이다
//   2. Create가 out 파라미터로 채운다. 값 반환이면 타입마다 move 생성자가 필요해진다
//   3. 소멸자는 인자를 못 받으므로 파괴에 필요한 것(dev/inst)을 non-owning으로 든다
//   4. 복사 금지 - handle이 두 번 파괴된다

#include <volk.h>

#include <cstdio>

#define LOG(...)  std::fprintf(stderr, __VA_ARGS__)

// 요구사항
// ============================================================================

// Dynamic rendering이 1.3 core라서 1.3이다.
constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;

// 화면에 그리려면 있어야 하는 device extension.
// (VK_KHR_surface는 instance extension이다. 층이 다르다.)
constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// 버전을 지원해도 feature가 꺼져 있을 수 있어 따로 확인한다.
//
// Contract: 확인할 때와 켤 때 같은 값을 봐야 한다. 어긋나면 device는 만들어지고
//           draw에서 죽는다.
inline VkPhysicalDeviceVulkan13Features RequiredFeatures13() noexcept {
    VkPhysicalDeviceVulkan13Features features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.dynamicRendering = VK_TRUE;   // VkRenderPass/VkFramebuffer 없이 그린다
    features.synchronization2 = VK_TRUE;   // barrier/submit API 개정판
    return features;
}

// core 1.0 feature는 지금 하나도 요구하지 않는다. 요구하던 fillModeNonSolid는
// POLYGON_MODE_LINE 때문이었는데 그 값을 쓰는 pipeline이 없어졌고, 안 쓰는 기능
// 때문에 GPU를 탈락시키고 있었다.
//
// 다시 필요해지면 여기와 Device.cpp의 후보 검사 **양쪽**에 넣는다 - 확인할 때와
// 켤 때 같은 값을 봐야 하고, 어긋나면 device는 만들어지고 draw에서 죽는다.
