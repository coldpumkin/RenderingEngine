#include "Vulkan/Buffer.h"

#include <cstring>

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
    // 스펙: EXCLUSIVE 자원을 다른 queue family가 ownership transfer 없이 읽으면
    // 내용이 정의되지 않는다. 현재 정책은 graphics queue만 쓰는 것이라 넘길 일이 없다.
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memoryUsage;
    allocInfo.flags = flags;

    // Buffer 생성 + memory type 선택 + 할당 + binding을 한 번에. 수동으로는
    // 다섯 단계였다(Buffer.h 참고).
    VmaAllocationInfo allocated{};
    const VkResult created = vmaCreateBuffer(dev.allocator, &bufferInfo, &allocInfo,
                                             &buffer.handle, &buffer.allocation, &allocated);
    if (created != VK_SUCCESS) {
        LOG("[vk] vmaCreateBuffer failed (%d, %llu bytes)\n",
            created, static_cast<unsigned long long>(size));
        return false;
    }

    // MAPPED_BIT을 줬으면 CPU 주소가 들어온다. 아니면 nullptr.
    buffer.mapped = allocated.pMappedData;
    buffer.size = size;
    return true;
}

Buffer::~Buffer() {
    if (dev == nullptr || handle == VK_NULL_HANDLE) { return; }
    // Buffer와 allocation을 한 번에. 수동일 때 지켜야 했던 순서도 VMA 안으로 들어갔다.
    vmaDestroyBuffer(dev->allocator, handle, allocation);
}

bool CreateDeviceLocalBuffer(const VulkanDevice& dev,
                             const Commands& commands,
                             const void* data,
                             VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             Buffer* out) noexcept {
    // Staging: CPU가 쓸 수 있는 임시 buffer. 지역 변수라 어느 경로로 나가든
    // ~Buffer가 정리한다.
    //
    // 수동일 때는 HOST_VISIBLE|HOST_COHERENT를 직접 지정하고 vkMapMemory/vkUnmapMemory를
    // 짝으로 불러야 했다. 그게 전부 아래 세 flag로 대체됐다.
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

    // 목적지: AUTO면 VMA가 DEVICE_LOCAL을 고른다. 그 메모리는 보통 CPU가 매핑할 수
    // 없어서(외장 GPU의 VRAM) staging을 거친다.
    //
    // usage flag는 여전히 우리가 말한다 - VMA가 대신하는 것은 "어느 메모리에 놓을까"이지
    // "무엇에 쓸 buffer인가"가 아니다.
    if (!CreateBuffer(dev, size,
                      usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      0,
                      out)) {
        return false;
    }

    // 절차는 texture upload와 같아서 Commands로 갈라져 나갔다.
    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }

    VkBufferCopy region{};
    region.size = size;
    dev.table.vkCmdCopyBuffer(cmd, staging.handle, out->handle, 1, &region);

    if (!EndOneShotAndWait(dev, commands, cmd, "device-local upload")) { return false; }
    // staging은 여기서 scope를 벗어나며 ~Buffer가 정리한다.

    LOG("[vk] vertex buffer ready (%llu bytes, device-local)\n",
        static_cast<unsigned long long>(size));
    return true;
}