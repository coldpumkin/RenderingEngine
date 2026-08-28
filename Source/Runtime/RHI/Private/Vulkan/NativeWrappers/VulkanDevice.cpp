#include "NativeWrappers/VulkanDevice.h"

#include "NativeWrappers/VulkanCommandPool.h"

#include <utility>

#include "RHILog.h"
#include "VulkanRequirements.h"
#include "VulkanResult.h"

namespace LambdaEngine {

std::unique_ptr<VulkanDevice> VulkanDevice::Create(
    const VulkanInstance& instance, const PhysicalDeviceSelection& selection) noexcept {

    if (selection.device == VK_NULL_HANDLE) {
        // 프로그래머 오류다. 선택에 실패했으면 여기까지 오면 안 된다 (D16).
        LAMBDA_LOG_ERROR("VulkanDevice::Create got an empty selection");
        return nullptr;
    }

    const VolkInstanceTable& it = instance.Table();

    // 큐는 vkCreateDevice가 만든다. 아래 CreateInfo가 "이 패밀리에서 1개 만들어달라"는 요청.
    // 우선순위는 같은 디바이스 안에서 큐끼리 경쟁할 때 쓰는 힌트인데, 큐가 하나뿐이라
    // 지금은 아무 의미가 없다. 그래도 pQueuePriorities는 필수 필드라 생략할 수 없다.
    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = selection.graphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    // **기능도 확장도 "지원 확인"과 "활성화"가 별개다.**
    // 확인은 선택기(VulkanPhysicalDevice.cpp)가 했고, 여기서는 "해달라"고 요청한다.
    // 둘이 같은 목록(VulkanRequirements.h)을 보므로 어긋날 수 없다.
    VkPhysicalDeviceVulkan13Features enable13 = RequiredVulkan13Features();

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &enable13;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount =
        static_cast<uint32_t>(std::size(kRequiredDeviceExtensions));
    deviceInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions;

    // vkCreateDevice는 **인스턴스 레벨 함수**다. 이 파일에 남은 유일한 인스턴스 레벨 호출이고,
    // "디바이스를 만드는 함수가 인스턴스 레벨"인 것은 Vulkan API의 사실이라 피할 수 없다.
    VkDevice device = VK_NULL_HANDLE;
    const VkResult result = it.vkCreateDevice(selection.device, &deviceInfo, nullptr, &device);
    if (result != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkCreateDevice failed: %s", ToString(result));
        return nullptr;
    }

    // 디바이스 레벨 함수를 테이블로 로드 (전역 아님).
    // 여기서 얻는 포인터는 로더의 디스패치 트램폴린을 건너뛰고 드라이버를 직접 가리킨다.
    //
    // 참고: volkLoadDeviceTable은 내부에서 **전역** vkGetDeviceProcAddr를 쓴다.
    // 그래서 VulkanInstance::Create가 volkLoadInstanceOnly도 같이 부른다.
    VolkDeviceTable table{};
    volkLoadDeviceTable(&table, device);

    VkPhysicalDeviceProperties props{};
    it.vkGetPhysicalDeviceProperties(selection.device, &props);
    LAMBDA_LOG_INFO("device: %s (Vulkan %u.%u, queue family %u, dynamicRendering + sync2 + swapchain)",
                    props.deviceName,
                    VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion),
                    selection.graphicsQueueFamily);

    // 여기까지 왔으면 모든 핸들이 유효하다. 생성자는 검증된 것만 받는다.
    return std::unique_ptr<VulkanDevice>(new VulkanDevice(selection, device, table));
}

VulkanDevice::VulkanDevice(const PhysicalDeviceSelection& selection,
                           VkDevice device,
                           const VolkDeviceTable& deviceTable) noexcept
    : device_(device),
      physicalDevice_(selection.device),
      graphicsQueueFamily_(selection.graphicsQueueFamily),
      table_(deviceTable) {

    // vkGetDeviceQueue는 이름과 달리 **큐를 만들지 않는다.** 조회다.
    //
    // 큐는 이미 vkCreateDevice가 VkDeviceQueueCreateInfo를 보고 다 만들어놨고,
    // 이 호출은 그중 (패밀리 인덱스, 큐 인덱스)로 지목해서 핸들만 꺼내온다.
    // 그래서 실패할 수도 없고(VkResult를 반환하지 않는다), 반납할 필요도 없다
    // (VkQueue에는 vkDestroy~가 없다. 디바이스가 죽을 때 같이 사라진다).
    table_.vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
}

void VulkanDevice::SetCommandPool(std::unique_ptr<VulkanCommandPool> pool) noexcept {
    commandPool_ = std::move(pool);
}

VulkanDevice::~VulkanDevice() {
    // **커맨드 풀을 먼저 지운다.** 이 디바이스로 만든 자식이라 vkDestroyDevice보다 먼저
    // 죽어야 하는데, 소멸자 본문은 멤버 파괴보다 먼저 실행되므로 그냥 두면
    // vkDestroyDevice가 먼저 돈다. 검증 레이어가 실제로 잡았다:
    //   "VkDevice has 2 leaked objects" (VkCommandBuffer, VkCommandPool)
    //
    // **소유가 안으로 들어오면 "죽기 전에 할 일"도 따라 들어온다** 와 같은 자리다.
    // 만든 것은 RHI지만 소유가 여기라서, 정리 책임도 여기다.
    commandPool_.reset();

    // 스펙상 vkDestroyDevice 전에 이 디바이스의 모든 큐 작업이 끝나 있어야 한다.
    //
    // 실패하면? 로그만 남기고 그대로 파괴를 진행한다. 소멸자에서는 그것 말고 할 게 없다
    // (Core Guidelines E.16). 여기서 멈추면 디바이스를 영영 반납 못 하고,
    // 던지면 프로그램이 죽는다. 둘 다 더 나쁘다.
    const VkResult idleResult = table_.vkDeviceWaitIdle(device_);
    if (idleResult != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkDeviceWaitIdle failed during teardown: %s", ToString(idleResult));
    }

    table_.vkDestroyDevice(device_, nullptr);
    LAMBDA_LOG_INFO("device destroyed");
}

} // namespace LambdaEngine
