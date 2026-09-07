#include "Vulkan/Timing.h"

#include <vector>   // the queue family query hands back an array

GpuTimer::~GpuTimer() {
    if (dev == nullptr || pool == VK_NULL_HANDLE) { return; }
    dev->table.vkDestroyQueryPool(dev->handle, pool, nullptr);
}

bool CreateGpuTimer(const VulkanDevice& dev, const Commands& commands, uint32_t count,
                    GpuTimer* out) noexcept {
    out->dev = &dev;
    out->capacity = count;

    // Two instance-level questions, both about this GPU rather than about any pool.
    VkPhysicalDeviceProperties props{};
    dev.inst->table.vkGetPhysicalDeviceProperties(dev.gpu, &props);
    out->period = props.limits.timestampPeriod;

    uint32_t familyCount = 0;
    dev.inst->table.vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &familyCount,
                                                             nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    dev.inst->table.vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &familyCount,
                                                             families.data());

    // The graphics family, because that is the queue every frame is submitted to. A
    // family may fill fewer than 64 bits, and the top bits are then undefined rather
    // than zero -- so they are masked off rather than trusted.
    const uint32_t bits = dev.families.graphics < familyCount
                        ? families[dev.families.graphics].timestampValidBits
                        : 0;
    if (bits == 0 || out->period <= 0.0f) {
        LOG("[vk] no timestamps on the graphics queue; pass timing is off\n");
        return true;
    }
    out->validMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    if (dev.table.vkCreateQueryPool(dev.handle, &info, nullptr, &out->pool)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateQueryPool failed\n");
        return false;
    }

    // A query that has never been reset cannot be read at all, and the first frame reads
    // this pool before its own reset has run. One command puts every query into the
    // reset-and-unwritten state, which reads back as unavailable.
    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }
    dev.table.vkCmdResetQueryPool(cmd, out->pool, 0, count);
    return EndOneShotAndWait(dev, commands, cmd, "query pool reset");
}

void ResetGpuTimer(const GpuTimer& timer, VkCommandBuffer cmd) noexcept {
    if (timer.pool == VK_NULL_HANDLE) { return; }

    // Every query, not the ones about to be written: a query left over from a frame that
    // took the other path would otherwise still report its old value as available.
    timer.dev->table.vkCmdResetQueryPool(cmd, timer.pool, 0, timer.capacity);
}

void WriteGpuTimestamp(const GpuTimer& timer, VkCommandBuffer cmd, uint32_t index,
                       VkPipelineStageFlags2 stage) noexcept {
    if (timer.pool == VK_NULL_HANDLE || index >= timer.capacity) { return; }
    timer.dev->table.vkCmdWriteTimestamp2(cmd, stage, timer.pool, index);
}

bool ReadGpuTimestamp(const GpuTimer& timer, uint32_t index, uint64_t* ticks) noexcept {
    if (timer.pool == VK_NULL_HANDLE || index >= timer.capacity) { return false; }

    // Value and availability together. Without the second one an unwritten query reads
    // as whatever the driver left there, and a pass that did not run would report a
    // time. WAIT is deliberately not set: this is called after the fence for the submit
    // that wrote them, so anything not ready is a query nobody wrote.
    uint64_t result[2]{};
    const VkResult read = timer.dev->table.vkGetQueryPoolResults(
        timer.dev->handle, timer.pool, index, 1, sizeof(result), result,
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (read != VK_SUCCESS || result[1] == 0) { return false; }

    *ticks = result[0] & timer.validMask;
    return true;
}

double GpuMillis(const GpuTimer& timer, uint64_t begin, uint64_t end) noexcept {
    if (timer.pool == VK_NULL_HANDLE || end <= begin) { return 0.0; }
    return static_cast<double>(end - begin) * static_cast<double>(timer.period) / 1.0e6;
}
