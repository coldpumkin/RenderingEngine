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
    // 명시적으로 넘겨야 하고, 안 넘기면 **내용이 정의되지 않는다**(스펙).
    // CreateVertexBuffer가 전송 큐로 올리면서 실제로 그 이전을 한다.
    //
    // 대안은 CONCURRENT지만 드라이버가 최적화를 포기하는 대가가 있다.
    // 이전이 초기화 때 한 번이면 EXCLUSIVE가 맞다.
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
    // **전용 전송 큐가 있으면 그쪽으로 올린다.**
    //
    // 속도 때문이 아니다 - 이건 루프 전에 한 번뿐이라 겹칠 대상이 없다. 이유는 둘:
    //   1. 전송 큐와 풀을 만들어만 놓고 **한 번도 제출한 적이 없었다.** 안 밟은 경로다
    //   2. 큐가 둘이 되면 **큐 패밀리 소유권 이전**이 필요해진다. EXCLUSIVE로 만든
    //      자원은 다른 패밀리가 그냥 읽으면 **내용이 정의되지 않는다**(스펙).
    //
    // 전용 큐가 없으면 그래픽스로 떨어지고, 그때는 이전도 필요 없다.
    const bool crossQueue = dev.families.HasTransfer();
    const VkQueue uploadQueue = crossQueue ? dev.queues.transfer : dev.queues.graphics;
    const VkCommandPool uploadPool = crossQueue ? commands.transfer : commands.graphics;
    const uint32_t srcFamily = crossQueue ? dev.families.transfer : VK_QUEUE_FAMILY_IGNORED;
    const uint32_t dstFamily = crossQueue ? dev.families.graphics : VK_QUEUE_FAMILY_IGNORED;

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    // 업로드용 하나. 큐가 둘이면 acquire를 기록할 그래픽스용이 하나 더 필요하다.
    // **풀은 큐 패밀리에 묶인다** - 다른 패밀리에 제출할 버퍼를 여기서 뽑을 수 없다.
    VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
    VkCommandBuffer acquireCmd = VK_NULL_HANDLE;
    VkSemaphore handoff = VK_NULL_HANDLE;

    allocInfo.commandPool = uploadPool;
    if (dev.table.vkAllocateCommandBuffers(dev.handle, &allocInfo, &uploadCmd) != VK_SUCCESS) {
        LOG("[vk] vkAllocateCommandBuffers(upload) failed\n");
        return false;
    }

    // 실패 경로가 여러 자원을 반납해야 해서 람다로 묶었다.
    const auto fail = [&](const char* what) {
        LOG("[vk] %s failed (vertex upload)\n", what);
        if (handoff != VK_NULL_HANDLE) {
            dev.table.vkDestroySemaphore(dev.handle, handoff, nullptr);
        }
        if (acquireCmd != VK_NULL_HANDLE) {
            dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &acquireCmd);
        }
        dev.table.vkFreeCommandBuffers(dev.handle, uploadPool, 1, &uploadCmd);
        return false;
    };

    // 버퍼 배리어 하나. crossQueue면 srcFamily/dstFamily가 실제 값이라 **소유권 이전**이
    // 되고, 아니면 IGNORED라 순수 메모리 배리어가 된다.
    //
    // release(전송 큐)와 acquire(그래픽스 큐)는 **패밀리 값이 정확히 같아야** 짝이 된다.
    // release는 dst 스코프를, acquire는 src 스코프를 무시한다(스펙).
    const auto bufferBarrier = [&](VkCommandBuffer cmd,
                                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.srcQueueFamilyIndex = srcFamily;
        barrier.dstQueueFamilyIndex = dstFamily;
        barrier.buffer = out->handle;
        barrier.size = VK_WHOLE_SIZE;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &barrier;
        dev.table.vkCmdPipelineBarrier2(cmd, &dep);
    };

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    // ---- 3a. 복사 + release ----
    if (dev.table.vkBeginCommandBuffer(uploadCmd, &beginInfo) != VK_SUCCESS) {
        return fail("vkBeginCommandBuffer(upload)");
    }
    VkBufferCopy region{};
    region.size = size;
    dev.table.vkCmdCopyBuffer(uploadCmd, staging.handle, out->handle, 1, &region);

    // crossQueue면 release: dst 스코프는 무시되므로 NONE.
    // 아니면 같은 큐 안의 메모리 배리어라 dst를 정점 읽기로 채운다.
    bufferBarrier(uploadCmd,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  crossQueue ? VK_PIPELINE_STAGE_2_NONE
                             : VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                  crossQueue ? VK_ACCESS_2_NONE
                             : VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);

    if (dev.table.vkEndCommandBuffer(uploadCmd) != VK_SUCCESS) {
        return fail("vkEndCommandBuffer(upload)");
    }

    // ---- 3b. crossQueue면 acquire를 그래픽스 쪽에 기록 ----
    if (crossQueue) {
        allocInfo.commandPool = commands.graphics;
        if (dev.table.vkAllocateCommandBuffers(dev.handle, &allocInfo, &acquireCmd)
                != VK_SUCCESS) {
            return fail("vkAllocateCommandBuffers(acquire)");
        }
        if (dev.table.vkBeginCommandBuffer(acquireCmd, &beginInfo) != VK_SUCCESS) {
            return fail("vkBeginCommandBuffer(acquire)");
        }
        // acquire: src 스코프는 무시되므로 NONE. 이 버퍼는 정점 입력으로 읽힌다.
        bufferBarrier(acquireCmd,
                      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                      VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        if (dev.table.vkEndCommandBuffer(acquireCmd) != VK_SUCCESS) {
            return fail("vkEndCommandBuffer(acquire)");
        }

        // **release가 끝나야 acquire를 시작할 수 있다.** 큐가 다르므로 배리어로는
        // 안 되고 세마포어가 필요하다 - 큐 사이를 잇는 것은 세마포어뿐이다.
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (dev.table.vkCreateSemaphore(dev.handle, &semInfo, nullptr, &handoff) != VK_SUCCESS) {
            return fail("vkCreateSemaphore(handoff)");
        }
    }

    // ---- 3c. 제출 ----
    // 초기화 경로라 기다려도 된다. 매 프레임이면 펜스로 넘겨받아야 한다.
    VkCommandBufferSubmitInfo uploadCmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    uploadCmdInfo.commandBuffer = uploadCmd;

    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = handoff;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

    VkSubmitInfo2 uploadSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    uploadSubmit.commandBufferInfoCount = 1;
    uploadSubmit.pCommandBufferInfos = &uploadCmdInfo;
    if (crossQueue) {
        uploadSubmit.signalSemaphoreInfoCount = 1;
        uploadSubmit.pSignalSemaphoreInfos = &signalInfo;
    }

    if (dev.table.vkQueueSubmit2(uploadQueue, 1, &uploadSubmit, VK_NULL_HANDLE) != VK_SUCCESS) {
        return fail("vkQueueSubmit2(upload)");
    }

    if (crossQueue) {
        VkCommandBufferSubmitInfo acquireCmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        acquireCmdInfo.commandBuffer = acquireCmd;

        VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitInfo.semaphore = handoff;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;

        VkSubmitInfo2 acquireSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        acquireSubmit.waitSemaphoreInfoCount = 1;
        acquireSubmit.pWaitSemaphoreInfos = &waitInfo;
        acquireSubmit.commandBufferInfoCount = 1;
        acquireSubmit.pCommandBufferInfos = &acquireCmdInfo;

        if (dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &acquireSubmit, VK_NULL_HANDLE)
                != VK_SUCCESS) {
            return fail("vkQueueSubmit2(acquire)");
        }
        if (dev.table.vkQueueWaitIdle(dev.queues.graphics) != VK_SUCCESS) {
            return fail("vkQueueWaitIdle(graphics)");
        }
    }
    // 업로드 큐도 비운다. 세마포어와 커맨드 버퍼를 놓으려면 둘 다 끝나 있어야 한다.
    if (dev.table.vkQueueWaitIdle(uploadQueue) != VK_SUCCESS) {
        return fail("vkQueueWaitIdle(upload)");
    }

    if (handoff != VK_NULL_HANDLE) {
        dev.table.vkDestroySemaphore(dev.handle, handoff, nullptr);
    }
    if (acquireCmd != VK_NULL_HANDLE) {
        dev.table.vkFreeCommandBuffers(dev.handle, commands.graphics, 1, &acquireCmd);
    }
    dev.table.vkFreeCommandBuffers(dev.handle, uploadPool, 1, &uploadCmd);
    // staging은 여기서 스코프를 벗어나며 ~Buffer가 정리한다.

    LOG("[vk] vertex buffer ready (%llu bytes, device-local, %s queue)\n",
        static_cast<unsigned long long>(size), crossQueue ? "transfer" : "graphics");
    return true;
}
