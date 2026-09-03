#pragma once

#include "Vulkan/Commands.h"
#include "Vulkan/Device.h"

// Buffer - the first place we choose memory ourselves
// ============================================================================
//
// vmaCreateBuffer stands in for vkCreateBuffer + requirements query + memory type
// match + vkAllocateMemory + vkBindBufferMemory. Not for the line count:
// vkAllocateMemory has a call cap (maxMemoryAllocationCount, often 4096) and VMA
// suballocates from large blocks, and the memory type policy leaves our code.
//
// Cost: VK_NO_PROTOTYPES hides the symbols, so CreateDevice fills VmaVulkanFunctions.
struct Buffer {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkBuffer handle = VK_NULL_HANDLE;

    // A slice of a VMA block, not a VkDeviceMemory. VMA knows the offset.
    VmaAllocation allocation = VK_NULL_HANDLE;

    // CPU address, non-null only with HOST_VISIBLE + MAPPED_BIT.
    void* mapped = nullptr;

    VkDeviceSize size = 0;

    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // Movable for the reason Image is: a Material owns one and materials live in a
    // vector. The source is left empty, so its destructor frees nothing.
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
};

// memoryUsage says who touches it and VMA picks the memory type:
//   AUTO              GPU reads it   -> prefers DEVICE_LOCAL
//   AUTO_PREFER_HOST  CPU writes it  -> prefers HOST_VISIBLE
// flags: HOST_ACCESS_SEQUENTIAL_WRITE, plus MAPPED_BIT for an out->mapped address.
bool CreateBuffer(const VulkanDevice& dev,
                  VkDeviceSize size,
                  VkBufferUsageFlags usage,
                  VmaMemoryUsage memoryUsage,
                  VmaAllocationCreateFlags flags,
                  Buffer* out) noexcept;

// Effect: uploads through a staging buffer, because DEVICE_LOCAL is what the GPU
//         reads fastest and usually what the CPU cannot map. Blocks until the copy
//         finishes - init path.
//
// usage is an argument because vertex and index upload differ in that one value and
// nothing else. TRANSFER_DST_BIT is added inside.
bool CreateDeviceLocalBuffer(const VulkanDevice& dev,
                             const Commands& commands,
                             const void* data,
                             VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             Buffer* out) noexcept;
