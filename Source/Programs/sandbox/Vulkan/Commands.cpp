#include "Vulkan/Commands.h"

#include <initializer_list>


VkCommandPool CreateCommandPool(const VulkanDevice& dev, uint32_t queueFamily) noexcept {
    // RESET_COMMAND_BUFFER lets one buffer rewind on its own. Without it the only
    // way back is resetting the whole pool, which would take every frame's buffer
    // with it.
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
    // The buffers go with the pool, so none are freed here.
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
    // Every result is checked. Unchecked, a copy that never reached the GPU still
    // printed its "ready" line and returned true.
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
    // A failed wait leaves the work in an unknown state. Freeing the buffer is unsafe
    // too -- the GPU may still be reading it -- and is the best available from here.
    if (dev.table.vkQueueWaitIdle(dev.queues.graphics) != VK_SUCCESS) {
        return fail("vkQueueWaitIdle");
    }

    dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &cmd);
    return true;
}
