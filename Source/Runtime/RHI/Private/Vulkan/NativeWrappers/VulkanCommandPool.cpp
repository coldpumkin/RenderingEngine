#include "NativeWrappers/VulkanCommandPool.h"

#include "NativeWrappers/VulkanDevice.h"

#include "RHILog.h"
#include "VulkanResult.h"

namespace LambdaEngine {

std::unique_ptr<VulkanCommandPool> VulkanCommandPool::Create(
    const VulkanDevice& device) noexcept {

    const VolkDeviceTable& table = device.Table();
    const VkDevice handle = device.Handle();

    // RESET_COMMAND_BUFFER: 버퍼 하나만 개별 리셋할 수 있게 한다. 버퍼가 하나뿐인
    // 지금은 차이가 없지만 매 프레임 같은 버퍼를 다시 기록하는 패턴을 드러낸다.
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device.GraphicsQueueFamily();

    VkCommandPool pool = VK_NULL_HANDLE;
    const VkResult poolResult = table.vkCreateCommandPool(handle, &poolInfo, nullptr, &pool);
    if (poolResult != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkCreateCommandPool failed: %s", ToString(poolResult));
        return nullptr;
    }

    // PRIMARY: 큐에 직접 제출할 수 있다. SECONDARY는 다른 버퍼 안에서만 실행된다.
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer buffer = VK_NULL_HANDLE;
    const VkResult allocResult = table.vkAllocateCommandBuffers(handle, &allocInfo, &buffer);
    if (allocResult != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkAllocateCommandBuffers failed: %s", ToString(allocResult));
        table.vkDestroyCommandPool(handle, pool, nullptr);
        return nullptr;
    }

    LAMBDA_LOG_INFO("command pool ready");

    return std::unique_ptr<VulkanCommandPool>(
        new VulkanCommandPool(device, pool, buffer));
}

VulkanCommandPool::VulkanCommandPool(const VulkanDevice& device,
                                           VkCommandPool pool,
                                           VkCommandBuffer buffer) noexcept
    : device_(device), pool_(pool), buffer_(buffer) {}

VulkanCommandPool::~VulkanCommandPool() {
    // 커맨드 버퍼를 GPU가 아직 실행 중일 수 있다. 자기 자원은 자기가 안전하게 파괴한다
    // - 디바이스가 소유하지만 대신 기다려주기를 기대하지 않는다.
    device_.WaitIdle();

    // buffer_는 따로 반납하지 않는다. 풀을 파괴하면 같이 사라진다.
    device_.Table().vkDestroyCommandPool(device_.Handle(), pool_, nullptr);

    LAMBDA_LOG_INFO("command pool destroyed");
}

void VulkanCommandPool::Reset() const noexcept {
    device_.Table().vkResetCommandBuffer(buffer_, 0);
}

void VulkanCommandPool::Begin() const noexcept {
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    device_.Table().vkBeginCommandBuffer(buffer_, &info);
}

void VulkanCommandPool::End() const noexcept {
    device_.Table().vkEndCommandBuffer(buffer_);
}

} // namespace LambdaEngine
