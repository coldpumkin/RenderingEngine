#include "Vulkan/Buffer.h"

#include <cstring>
#include <new>      // placement new in move assignment

bool CreateBuffer(const VulkanDevice& dev,
                  VkDeviceSize size,
                  VkBufferUsageFlags usage,
                  VmaMemoryUsage memoryUsage,
                  VmaAllocationCreateFlags flags,
                  Buffer* out) noexcept {
    Buffer& buffer = *out;
    buffer.dev = &dev;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    // Spec: an EXCLUSIVE resource read by another queue family without an ownership
    // transfer has undefined contents. Everything here stays on graphics.
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memoryUsage;
    allocInfo.flags = flags;

    VmaAllocationInfo allocated{};
    const VkResult created = vmaCreateBuffer(dev.allocator, &bufferInfo, &allocInfo,
                                             &buffer.handle, &buffer.allocation, &allocated);
    if (created != VK_SUCCESS) {
        LOG("[vk] vmaCreateBuffer failed (%d, %llu bytes)\n",
            created, static_cast<unsigned long long>(size));
        return false;
    }

    buffer.mapped = allocated.pMappedData;
    buffer.size = size;
    return true;
}

Buffer::Buffer(Buffer&& other) noexcept
    : dev(other.dev), handle(other.handle), allocation(other.allocation),
      mapped(other.mapped), size(other.size) {
    other.dev = nullptr;
    other.handle = VK_NULL_HANDLE;
    other.allocation = VK_NULL_HANDLE;
    other.mapped = nullptr;
    other.size = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        this->~Buffer();
        new (this) Buffer(static_cast<Buffer&&>(other));
    }
    return *this;
}

Buffer::~Buffer() {
    if (dev == nullptr || handle == VK_NULL_HANDLE) { return; }
    vmaDestroyBuffer(dev->allocator, handle, allocation);
}

bool CreateDeviceLocalBuffer(const VulkanDevice& dev,
                             const Commands& commands,
                             const void* data,
                             VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             Buffer* out) noexcept {
    // A local, so ~Buffer frees it on every exit path below.
    Buffer staging;
    if (!CreateBuffer(dev, size,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                          | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                      &staging)) {
        return false;
    }
    if (staging.mapped == nullptr) {
        LOG("[vk] staging buffer is not mapped\n");
        return false;
    }
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));

    // AUTO lands on DEVICE_LOCAL. usage stays ours: VMA chooses where the memory is,
    // not what the buffer is for.
    if (!CreateBuffer(dev, size,
                      usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      0,
                      out)) {
        return false;
    }

    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }

    VkBufferCopy region{};
    region.size = size;
    dev.table.vkCmdCopyBuffer(cmd, staging.handle, out->handle, 1, &region);

    if (!EndOneShotAndWait(dev, commands, cmd, "device-local upload")) { return false; }

    LOG("[vk] buffer ready (%llu bytes, device-local)\n",
        static_cast<unsigned long long>(size));
    return true;
}
