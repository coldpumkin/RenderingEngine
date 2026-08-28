#pragma once

#include "NativeWindowHandle.h"
#include "NativeWrappers/VulkanInstance.h"

#include <volk.h>

#include <memory>
#include <vector>

namespace LambdaEngine {

// 이 서피스가 특정 GPU에 대해 무엇을 받아주는가.
//
// 스왑체인을 만들 때 능력과 포맷이 둘 다 필요해서 한 번에 묻는다.
// valid == false면 조회가 실패한 것이고, 실패 이유는 조회 지점이 이미 기록했다.
struct SurfaceSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    bool valid = false;
};

// VkSurfaceKHR 하나를 소유한다. Vulkan이 창에 그림을 내보내는 통로다.
//
// 인스턴스에서 만들고 인스턴스로 파괴하지만 인스턴스의 일부는 아니다 - 수명이 창에
// 묶이고, 변하는 이유도 창 시스템이다. 만드는 함수의 소속과 무엇의 일부인지는 다르다.
class VulkanSurface {
public:
    // instance를 참조로 받는다. 파괴할 때 인스턴스의 함수 테이블이 필요하다 (기준 B).
    static std::unique_ptr<VulkanSurface> Create(const VulkanInstance& instance,
                                                 NativeWindowHandle window) noexcept;

    ~VulkanSurface();

    VulkanSurface(const VulkanSurface&) = delete;
    VulkanSurface& operator=(const VulkanSurface&) = delete;
    VulkanSurface(VulkanSurface&&) = delete;
    VulkanSurface& operator=(VulkanSurface&&) = delete;

    VkSurfaceKHR Handle() const noexcept { return surface_; }

    // "이 창은 이 GPU에게서 무엇을 받나."
    //
    // 서피스가 답하는 이유: 하는 일 자체가 서피스 조회이므로 서피스가 주역이다.
    // 자유 함수로 두면 인스턴스 테이블을 얻으려고 Instance() 접근자가 필요해지고,
    // 그러면 서피스를 받은 쪽이 인스턴스에 닿게 된다 - 지금은 그럴 이유가 없다.
    //
    // GPU를 인자로 받는 이유: 조회는 (서피스, GPU) 쌍에 대한 것이고, 선택 단계에서는
    // 아직 VulkanDevice가 없어 후보 핸들만 존재한다. 래퍼가 없는 핸들이다 (D88).
    SurfaceSupport QuerySupport(VkPhysicalDevice physicalDevice) const noexcept;

    // "이 큐 패밀리가 **이 창**에 present 할 수 있는가."
    //
    // GPU 선택은 "이 플랫폼에 present 되는가"까지만 답할 수 있었다 - 그때는 창이 없었다.
    // 층이 다른 확인이고, 서피스가 생긴 뒤에만 가능하다 (D95).
    //
    // 위와 같은 이유로 서피스가 답한다: 하는 일 자체가 서피스 조회다.
    // 조회 실패와 "지원 안 함"을 구분하지 않는 이유 - 호출자가 할 일이 같다(생성 포기).
    bool SupportsPresent(VkPhysicalDevice physicalDevice, uint32_t queueFamily) const noexcept;

private:
    VulkanSurface(const VulkanInstance& instance, VkSurfaceKHR surface) noexcept;

    // 파괴에 필요한 비소유 상태(②). 참조라서 남의 것임이 타입에 드러난다.
    const VulkanInstance& instance_;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
};

} // namespace LambdaEngine
