#include "Vulkan/Commands.h"

#include <initializer_list>

// ============================================================================
// 6. 커맨드 풀 (큐 패밀리마다) + 프레임 자원 (frames-in-flight마다)
// ============================================================================

VkCommandPool CreateCommandPool(const VulkanDevice& dev, uint32_t queueFamily) noexcept {
    // RESET_COMMAND_BUFFER: 풀 전체가 아니라 버퍼 하나만 개별 리셋할 수 있게 한다.
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = queueFamily;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (dev.table.vkCreateCommandPool(dev.handle, &info, nullptr, &pool) != VK_SUCCESS) {
        LOG("[vk] vkCreateCommandPool failed (family %u)\n", queueFamily);
        return VK_NULL_HANDLE;
    }
    return pool;
}

bool CreateCommands(const VulkanDevice& dev, Commands* out) noexcept {
    out->dev = &dev;

    out->graphics = CreateCommandPool(dev, dev.families.graphics);
    if (out->graphics == VK_NULL_HANDLE) { return false; }

    if (dev.families.HasCompute()) {
        out->compute = CreateCommandPool(dev, dev.families.compute);
        if (out->compute == VK_NULL_HANDLE) { return false; }
    }
    if (dev.families.HasTransfer()) {
        out->transfer = CreateCommandPool(dev, dev.families.transfer);
        if (out->transfer == VK_NULL_HANDLE) { return false; }
    }
    return true;
}

Commands::~Commands() {
    if (dev == nullptr) { return; }
    // 풀을 파괴하면 거기서 나온 커맨드 버퍼도 같이 사라진다.
    for (VkCommandPool pool : {graphics, compute, transfer}) {
        if (pool != VK_NULL_HANDLE) {
            dev->table.vkDestroyCommandPool(dev->handle, pool, nullptr);
        }
    }
}
