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

VkCommandBuffer BeginOneShot(const VulkanDevice& dev, const Commands& commands) noexcept {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commands.graphics;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (dev.table.vkAllocateCommandBuffers(dev.handle, &allocInfo, &cmd) != VK_SUCCESS) {
        LOG("[vk] vkAllocateCommandBuffers(one-shot) failed\n");
        return VK_NULL_HANDLE;
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dev.table.vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        LOG("[vk] vkBeginCommandBuffer(one-shot) failed\n");
        dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

bool EndOneShotAndWait(const VulkanDevice& dev, const Commands& commands,
                       VkCommandBuffer cmd, const char* what) noexcept {
    // 반환값을 다 본다. 한때 전부 버렸는데 그러면 복사가 한 줄도 실행되지 않아도
    // "ready" log가 찍히고 true가 나갔다.
    const auto fail = [&](const char* step) {
        LOG("[vk] %s failed (%s)\n", step, what);
        dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &cmd);
        return false;
    };

    if (dev.table.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return fail("vkEndCommandBuffer");
    }

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;

    if (dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &submit, VK_NULL_HANDLE)
            != VK_SUCCESS) {
        return fail("vkQueueSubmit2");
    }
    // 대기가 실패하면 작업이 끝났는지 알 수 없다. Command buffer 반납도 위험하지만
    // (GPU가 아직 읽을 수 있다) 여기서 할 수 있는 최선이다.
    if (dev.table.vkQueueWaitIdle(dev.queues.graphics) != VK_SUCCESS) {
        return fail("vkQueueWaitIdle");
    }

    dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &cmd);
    return true;
}
