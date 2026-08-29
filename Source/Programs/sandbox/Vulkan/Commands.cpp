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

// ---------------------------------------------------------------------------
// 프레임 자원 - frames-in-flight마다 한 벌
// ---------------------------------------------------------------------------
//
// **셋이 같은 신호 하나에 묶인다.** 판별은 이렇다:
// *"이 자원을 다시 써도 된다는 걸 무엇이 알려주는가?"*
//
//   cmd             pending 상태면 리셋할 수 없다      -> inFlight 펜스가 알려준다
//   imageAvailable  이전 wait(submit)이 끝나야 재signal -> inFlight 펜스가 알려준다
//   inFlight        그 자신이 신호다
//
// 셋 다 답이 같은 펜스 하나다. 그래서 개수도 같고(= frames-in-flight) 한 벌이다.
//
// **renderFinished가 여기 없는 이유도 같은 기준이다.** 그건 present가 기다리는데,
// present에는 완료를 알려주는 것이 없다(vkQueuePresentKHR은 펜스를 주지 않는다).
// 유일한 단서가 "acquire가 그 이미지를 다시 줬다"이고 그건 이미지 인덱스로만 오므로,
// 개수가 이미지 수가 되어 Swapchain 안에 산다.
//
// 지금 frames-in-flight = 1이라 한 벌이다. 2로 올리면 이 struct가 배열이 되고,
// 루프는 frames[frameIndex]를 돌려쓰게 된다.
