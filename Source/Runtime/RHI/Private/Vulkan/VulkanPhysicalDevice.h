#pragma once

#include "NativeWrappers/VulkanInstance.h"

#include <volk.h>

#include <cstdint>

namespace LambdaEngine {

// 물리 디바이스(GPU)에 관한 것 - 고르기와 조회.
//
// VulkanDevice가 아니라 여기인 이유:
//   - 열거·조회가 전부 인스턴스 레벨 함수다
//   - VulkanDevice는 "이 디바이스"를 나타내는데 선택 단계에는 아직 그게 없다
//   - 선택 정책(요구 버전, 디스크리트 선호)은 GPU의 성질이 아니라 엔진의 정책이다
//
// 클래스가 아니라 자유 함수인 이유: 소유할 자원이 없다.
// vkDestroyPhysicalDevice는 존재하지 않는다. 조회가 더 늘면 클래스 승격을 검토한다.

// device == VK_NULL_HANDLE이면 실패. 값 타입이라 nullptr을 못 쓴다.
struct PhysicalDeviceSelection {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
};

// 요구사항(VulkanRequirements.h)을 만족하는 GPU 중 가장 좋은 것.
// 탈락한 후보는 반드시 이유를 로그로 남긴다.
//
// **창을 받지 않는다.** GPU를 고르는 것은 GPU backend 수명에 속하고, 그건 창이 하나도
// 없어도 성립해야 한다. present 능력은 `vkGetPhysicalDeviceWin32PresentationSupportKHR`로
// 서피스 없이 묻는다 - "이 큐 패밀리가 이 플랫폼의 창에 present 할 수 있는가"이며,
// 그것이 GPU의 자격 조건으로 물어야 할 전부다.
//
// **특정 창에 대한 확인은 여기 없다.** "이 서피스에 present 되는가"는 그 서피스가
// 생겼을 때 물어야 하고, 그건 window/presentation 수명의 일이다.
PhysicalDeviceSelection SelectPhysicalDevice(const VulkanInstance& instance) noexcept;

// 서피스 능력·포맷 조회는 여기 없다. 하는 일 자체가 서피스 조회라 주역이 서피스이고,
// `VulkanSurface::QuerySupport()`가 답한다. 여기 남는 것은 **정책**이다 -
// 무엇을 요구하고 어느 GPU를 더 좋게 볼 것인가. 그건 GPU의 성질이 아니라 엔진의 것이다.

} // namespace LambdaEngine
