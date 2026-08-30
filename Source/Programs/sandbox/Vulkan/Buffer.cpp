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

bool CreateVertexBuffer(const VulkanDevice& dev,
                        const Commands& commands,
                        const void* data,
                        VkDeviceSize size,
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
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                          | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      0,
                      out)) {
        return false;
    }

    // 현재 정책: graphics queue를 쓴다. Transfer queue의 값어치는 그리는 동안 동시에
    // 올리는 것인데 이건 루프 전에 한 번뿐이라 겹칠 대상이 없고, queue를 바꾸면
    // ownership transfer 비용만 낸다.
    //
    // 한 번 옮겨봤다가 되돌렸다(b104a27). 필요해지면 그 commit을 꺼내 쓴다 -
    // release/acquire barrier 한 쌍과 queue 사이를 잇는 semaphore가 거기 다 있다.
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commands.graphics;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (dev.table.vkAllocateCommandBuffers(dev.handle, &allocInfo, &cmd) != VK_SUCCESS) {
        LOG("[vk] vkAllocateCommandBuffers(upload) failed\n");
        return false;
    }

    // 반환값을 다 본다. 한때 전부 버렸는데 그러면 복사가 한 줄도 실행되지 않아도
    // "ready" log가 찍히고 true가 나갔다.
    //
    // 실패 경로가 command buffer를 반납해야 해서 lambda로 묶었다.
    const auto fail = [&](const char* what) {
        LOG("[vk] %s failed (vertex upload)\n", what);
        dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &cmd);
        return false;
    };

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dev.table.vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        return fail("vkBeginCommandBuffer");
    }

    VkBufferCopy region{};
    region.size = size;
    dev.table.vkCmdCopyBuffer(cmd, staging.handle, out->handle, 1, &region);

    if (dev.table.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return fail("vkEndCommandBuffer");
    }

    // 복사가 끝날 때까지 기다린다. 초기화 경로라 기다려도 된다 - 매 frame이면
    // fence로 넘겨받아야 한다.
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;

    if (dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &submit, VK_NULL_HANDLE)
            != VK_SUCCESS) {
        return fail("vkQueueSubmit2");
    }
    // 대기가 실패하면 복사가 끝났는지 알 수 없다. Command buffer 반납도 위험하지만
    // (GPU가 아직 읽을 수 있다) 여기서 할 수 있는 최선이다.
    if (dev.table.vkQueueWaitIdle(dev.queues.graphics) != VK_SUCCESS) {
        return fail("vkQueueWaitIdle");
    }

    dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &cmd);
    // staging은 여기서 scope를 벗어나며 ~Buffer가 정리한다.

    LOG("[vk] vertex buffer ready (%llu bytes, device-local)\n",
        static_cast<unsigned long long>(size));
    return true;
}