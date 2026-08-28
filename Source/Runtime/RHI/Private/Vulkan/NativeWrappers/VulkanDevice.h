#pragma once

#include "NativeWrappers/VulkanInstance.h"
#include "VulkanPhysicalDevice.h"

#include <volk.h>

#include <cstdint>
#include <memory>

namespace LambdaEngine {

// 디바이스가 소유한다. 참조 멤버가 아니라 unique_ptr이라 정의가 필요 없다.
class VulkanCommandPool;

// VkDevice 생성/파괴와 큐 확보. 생성 후에는 디바이스 레벨 전용이다.
//
// 물리 디바이스 선택은 여기 없다 - 인스턴스 레벨 작업이고 엔진 정책이라
// VulkanPhysicalDevice.h로 뺐다. 이 클래스는 고른 GPU로 디바이스를 만들기만 한다.
//
// 리소스(버퍼/텍스처)는 소유하지 않는다. 팩토리/큐 제공자 역할만 한다.
class VulkanDevice {
public:
    // instance를 참조로 받는 이유: vkCreateDevice가 인스턴스 레벨 함수다.
    // selection은 SelectPhysicalDevice의 결과다 - 고르는 일과 만드는 일을 나눴다.
    static std::unique_ptr<VulkanDevice> Create(const VulkanInstance& instance,
                                                const PhysicalDeviceSelection& selection) noexcept;

    ~VulkanDevice();

    // 이동을 허용하면 "핸들을 빼앗긴 빈 VulkanDevice"가 생기고 소멸자마다 null 검사가
    // 필요해진다. 이동 가능한 소유권이 필요하면 unique_ptr이 그 일을 한다.
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    VulkanDevice(VulkanDevice&&) = delete;
    VulkanDevice& operator=(VulkanDevice&&) = delete;

    // 전부 non-owning 반환. 여기는 Private/이고 은닉의 경계는 모듈이다 (D57).
    VkPhysicalDevice PhysicalDevice() const noexcept { return physicalDevice_; }
    VkDevice Handle() const noexcept { return device_; }
    uint32_t GraphicsQueueFamily() const noexcept { return graphicsQueueFamily_; }
    VkQueue GraphicsQueue() const noexcept { return graphicsQueue_; }
    const VolkDeviceTable& Table() const noexcept { return table_; }

    // 커맨드 풀·버퍼. 큐 패밀리에 묶이고 **이 디바이스와 함께 죽어야 하므로 소유는 여기**다.
    //
    // 다만 **만드는 것은 디바이스가 아니다** - 커맨드 풀 없이도 디바이스는 유효하므로
    // 생성 조건이 아니고, 그릴 준비가 될 때 밖에서 만들어 여기로 옮긴다.
    // 그 전까지 `commandPool_`은 nullptr이다.
    void SetCommandPool(std::unique_ptr<VulkanCommandPool> pool) noexcept;
    const VulkanCommandPool& CommandPool() const noexcept { return *commandPool_; }

    // 파괴 직전에 필요하다. VulkanDevice의 소멸자는 다른 멤버가 이미 파괴된 뒤라
    // 너무 늦으므로 VulkanRHI의 소멸자 본문에서 부른다.
    void WaitIdle() const noexcept { table_.vkDeviceWaitIdle(device_); }

private:
    VulkanDevice(const PhysicalDeviceSelection& selection,
                 VkDevice device,
                 const VolkDeviceTable& deviceTable) noexcept;

    // 인스턴스 참조를 들지 않는다. vkCreateDevice에만 필요하고 그건 Create()의 인자다.
    // 생성 후 하는 일(큐 조회·대기·파괴)은 전부 디바이스 레벨이다.

    VkDevice device_ = VK_NULL_HANDLE;

    // 이 디바이스의 정체(③) - 생성 시 확정, 불변, 디바이스가 죽으면 의미 상실.
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = 0;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;

    // 전역(volkLoadDevice)은 마지막 로드 대상으로 덮여 멀티 디바이스에서 조용히
    // 틀린 디바이스를 호출한다. 테이블은 디바이스마다 따로다. (D8)
    VolkDeviceTable table_{};

    std::unique_ptr<VulkanCommandPool> commandPool_;
};

} // namespace LambdaEngine
