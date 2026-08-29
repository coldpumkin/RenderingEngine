#include "Vulkan/Buffer.h"

#include <cstring>

// ============================================================================
// ============================================================================
// 9. 버퍼
// ============================================================================

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
    // EXCLUSIVE: 한 번에 한 큐 패밀리만 소유한다. 다른 패밀리가 쓰려면 소유권을
    // 명시적으로 넘겨야 한다. 지금은 그래픽스 큐만 만지므로 넘길 일이 없다.
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memoryUsage;
    allocInfo.flags = flags;

    // **한 번에 셋을 다 한다**: 버퍼 생성 + 메모리 타입 선택 + 할당 + 바인딩.
    // 수동으로는 vkCreateBuffer / vkGetBufferMemoryRequirements / 타입 대조 /
    // vkAllocateMemory / vkBindBufferMemory 다섯 단계였다.
    VmaAllocationInfo allocated{};
    const VkResult created = vmaCreateBuffer(dev.allocator, &bufferInfo, &allocInfo,
                                             &buffer.handle, &buffer.allocation, &allocated);
    if (created != VK_SUCCESS) {
        LOG("[vk] vmaCreateBuffer failed (%d, %llu bytes)\n",
            created, static_cast<unsigned long long>(size));
        return false;
    }

    // MAPPED_BIT을 줬으면 여기 CPU 주소가 들어온다. 아니면 nullptr.
    buffer.mapped = allocated.pMappedData;
    buffer.size = size;
    return true;
}

Buffer::~Buffer() {
    if (dev == nullptr || handle == VK_NULL_HANDLE) { return; }
    // **버퍼와 할당을 한 번에 놓는다.** 수동일 때는 순서(버퍼 먼저, 메모리 나중)를
    // 지켜야 했는데 그 순서도 VMA 안으로 들어갔다.
    vmaDestroyBuffer(dev->allocator, handle, allocation);
}

bool CreateVertexBuffer(const VulkanDevice& dev,
                        const Commands& commands,
                        const void* data,
                        VkDeviceSize size,
                        Buffer* out) noexcept {
    // ---- 1. 스테이징: CPU가 쓸 수 있는 임시 버퍼 ----
    //
    // **무엇을 원하는지만 말한다.** 어떤 메모리 타입인지는 VMA가 고른다:
    //   AUTO_PREFER_HOST              CPU가 자주 만지니 시스템 RAM 쪽을 선호해라
    //   HOST_ACCESS_SEQUENTIAL_WRITE  CPU가 순차로 쓰기만 한다 (읽지 않는다)
    //   MAPPED_BIT                    매핑을 유지해라 -> staging.mapped에 주소가 온다
    //
    // 수동일 때는 HOST_VISIBLE|HOST_COHERENT를 직접 지정하고, vkMapMemory와
    // vkUnmapMemory를 짝으로 불러야 했다. 그게 전부 위 세 플래그로 대체됐다.
    //
    // **지역 변수다.** 어느 경로로 나가든 ~Buffer가 정리한다.
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

    // ---- 2. 목적지: GPU가 빠르게 읽는 메모리 ----
    //
    // VMA_MEMORY_USAGE_AUTO = "GPU가 주로 쓴다". VMA가 DEVICE_LOCAL을 고른다.
    // 그 메모리는 보통 CPU가 매핑할 수 없어서(외장 GPU의 VRAM) 스테이징을 거친다.
    // TRANSFER_DST: 복사의 목적지가 될 수 있다고 미리 알려준다.
    //
    // **usage 플래그(무엇에 쓰는 버퍼인가)는 여전히 우리가 말한다.** VMA가 대신하는 것은
    // "어느 메모리에 놓을까"이지 "무엇에 쓸 버퍼인가"가 아니다.
    if (!CreateBuffer(dev, size,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                          | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      0,
                      out)) {
        return false;
    }

    // ---- 3. GPU에게 복사를 시킨다 ----
    //
    // **그래픽스 큐를 쓴다. 전송 큐가 아니다.**
    // 전송 큐의 값어치는 그리는 동안 **동시에** 올리는 것인데, 이건 루프가 시작하기 전
    // 한 번뿐이라 겹칠 대상이 없다. 반면 큐를 바꾸면 **큐 패밀리 소유권 이전**
    // (release/acquire 배리어 한 쌍)이 필요해진다 - 얻는 것 없이 비용만 낸다.
    //
    // 전송 큐는 **그리는 중에 올려야 할 때** 값을 한다. 그때 옮긴다.
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commands.graphics;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (dev.table.vkAllocateCommandBuffers(dev.handle, &allocInfo, &cmd) != VK_SUCCESS) {
        LOG("[vk] vkAllocateCommandBuffers(upload) failed\n");
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev.table.vkBeginCommandBuffer(cmd, &beginInfo);

    VkBufferCopy region{};
    region.size = size;
    dev.table.vkCmdCopyBuffer(cmd, staging.handle, out->handle, 1, &region);

    dev.table.vkEndCommandBuffer(cmd);

    // 복사가 끝날 때까지 기다린다. **초기화 경로라 기다려도 된다** -
    // 매 프레임이면 펜스로 넘겨받아야 하지만 여기는 한 번뿐이다.
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;

    dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &submit, VK_NULL_HANDLE);
    dev.table.vkQueueWaitIdle(dev.queues.graphics);

    dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &cmd);
    // staging은 여기서 스코프를 벗어나며 ~Buffer가 정리한다.

    LOG("[vk] vertex buffer ready (%llu bytes, device-local)\n",
        static_cast<unsigned long long>(size));
    return true;
}