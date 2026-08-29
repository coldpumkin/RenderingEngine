#pragma once

// 모든 Vulkan 파일이 공유하는 것 - 로그, 요구사항, 그리고 **구조의 가장 큰 선**.
//
// ---------------------------------------------------------------------------
// **Vulkan 호출은 예외 없이 두 층으로 갈린다.**
//
//   인스턴스 레벨   VkInstance 또는 **VkPhysicalDevice**로 디스패치
//                   (물리 디바이스 조회는 인스턴스 핸들이 필요 없다)
//   디바이스 레벨   VkDevice · VkQueue · **VkCommandBuffer**로 디스패치
//                   (vkCmd*는 디바이스 핸들이 필요 없다)
//
// 우리가 정한 선이 아니라 API 자체의 선이고, 파일이 이 선을 따라 나뉜다.
// 각 층은 자기 함수 테이블(VolkInstanceTable / VolkDeviceTable)을 들고,
// 핸들과 테이블이 한 객체에서 나와야 섞일 수 없다.
//
// **전역으로 남는 것은 부트스트랩 4개뿐이다** - volkInitialize,
// vkEnumerateInstanceVersion, vkEnumerateInstanceLayerProperties, vkCreateInstance.
// 인스턴스가 없을 때 부르는 함수라 테이블에 담을 수가 없다.
// ---------------------------------------------------------------------------
//
// **RAII 규약** (자원 타입 전부에 적용)
//   1. 기본 생성 = 비어 있음. 그 상태가 합법이다
//   2. Create가 out 파라미터로 채운다. 값 반환이 아니다 - 소멸자가 있는 타입을 값으로
//      반환하면 이동 생성자가 타입마다 필요해진다
//   3. 소멸자는 인자를 못 받으므로 파괴에 필요한 것(dev 또는 inst)을 비소유 포인터로 든다
//   4. 복사 금지 - 핸들이 두 번 파괴된다

#include <volk.h>

#include <cstdio>

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