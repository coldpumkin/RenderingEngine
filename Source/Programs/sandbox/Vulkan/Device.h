#pragma once

#include "Vulkan/Instance.h"

#include <vma/vk_mem_alloc.h>

// Physical device 고르기 + queue family 고르기
// ============================================================================
//
// GPU는 queue를 family로 묶어서 "이 그룹은 graphics+compute+transfer를 다 하고,
// 저 그룹은 transfer만 전담한다"고 알려준다. Transfer 전담 family는 보통 별도 DMA
// engine이라 graphics와 물리적으로 병렬로 돈다.
//
// 스펙: GRAPHICS나 COMPUTE bit가 있으면 transfer는 암묵적으로 지원된다. 그래서
// "transfer가 되나"를 물으려고 TRANSFER bit를 보면 안 되고, "transfer만 하는 전용
// family인가"를 물을 때 본다.
//
// 데스크톱 GPU의 전형:
//   family 0 : GRAPHICS | COMPUTE | TRANSFER   범용
//   family 1 : COMPUTE  | TRANSFER             async compute
//   family 2 : TRANSFER                        DMA engine
struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;   // 필수. present도 여기서 한다
    uint32_t compute  = UINT32_MAX;   // 없을 수 있다
    uint32_t transfer = UINT32_MAX;   // 없을 수 있다

    bool HasCompute()  const noexcept { return compute  != UINT32_MAX; }
    bool HasTransfer() const noexcept { return transfer != UINT32_MAX; }
};

// 현재 정책: 전용 family가 아니면 안 만든다.
//
// Compute/transfer queue를 따로 두는 목적은 graphics와 동시에 도는 것이다. 같은
// family로 대체하면 그 이득은 없으면서 queue가 갈리는 비용(semaphore, queue family
// ownership transfer)은 그대로 낸다. Unreal도 전용을 못 찾으면 null로 둔다.
//
// Graphics만 못 찾으면 실패다.
struct PhysicalDeviceSelection {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    QueueFamilies families;
};

// 자격을 통과한 것 중 외장을 선호한다.
// 실패하면 gpu가 VK_NULL_HANDLE인 채로 돌아온다.
PhysicalDeviceSelection PickPhysicalDevice(const VulkanInstance& inst,
                                           VkSurfaceKHR surface) noexcept;

// Logical device + function table + queues
// ============================================================================

// 만든 queue handle들. compute/transfer는 VK_NULL_HANDLE일 수 있다 - 전용 family가
// 없다는 뜻이고, 그때 그 일은 graphics가 한다.
struct Queues {
    VkQueue graphics = VK_NULL_HANDLE;
    VkQueue compute  = VK_NULL_HANDLE;
    VkQueue transfer = VK_NULL_HANDLE;

    // Present는 4번째 queue가 아니라 역할이다. 위 셋 중 하나를 가리키는 별칭이고
    // 기본은 graphics다. Unreal도 같다(FVulkanQueue* PresentQueue).
    //
    // 현재 정책: graphics가 present를 못 하는 하드웨어는 지원하지 않는다. Unreal도
    // 그 경우 메시지박스를 띄우고 종료한다.
    //
    // 나중에 볼 것: AMD에서 compute queue로 present하는 빠른 경로가 있다. 제출 구조가
    // 바뀌므로 지연을 실제로 잴 수 있을 때 검토한다.
    VkQueue present = VK_NULL_HANDLE;

    VkQueue ComputeOrGraphics()  const noexcept { return compute  ? compute  : graphics; }
    VkQueue TransferOrGraphics() const noexcept { return transfer ? transfer : graphics; }
};

// Device 층. 다섯이 한 몸이다.
//
// 근거: device가 생긴 뒤로 gpu · families · queues가 device 없이 쓰이는 곳이 없다.
// 반대로 device를 쓰는 곳은 거의 다 table도 같이 쓴다.
struct VulkanDevice {
    VolkDeviceTable table{};
    VkDevice handle = VK_NULL_HANDLE;

    // 선택 결과가 여기로 흡수됐다. 파괴할 것이 없는 값이라 소유가 아니다.
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    QueueFamilies families;

    // vkGetDeviceQueue는 조회다. vkCreateDevice가 이미 만들었고 파괴 함수도 없다.
    Queues queues;

    // 이 GPU의 memory type 목록. 생성 시 확정 · 불변 · device가 죽으면 의미 상실이라
    // 여기 있다. 조회 함수가 instance level이라 나중에 다시 물으려면 instance가 필요한데,
    // buffer를 만들 때마다 instance를 끌고 다니는 대신 한 번 담아둔다.
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    // Depth format은 여기 없다 - 후보 목록과 우선순위는 우리 render target의 정책이지
    // GPU의 성질이 아니다. RenderTargets.h의 ChooseRenderTargetFormats에 있다.

    // GPU memory allocator. Device가 만들고 device와 함께 죽는다.
    VmaAllocator allocator = VK_NULL_HANDLE;

    VulkanDevice() = default;
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
};

// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// inst를 받는 이유: vkCreateDevice는 instance level 함수다. 만드는 함수와 파괴하는
// 함수(vkDestroyDevice, device level)의 층이 다르다는 API의 비대칭이고, 그래서
// "이 타입이 무슨 level이냐"가 아니라 "이 호출이 무슨 level이냐"로 봐야 한다.
bool CreateDevice(const VulkanInstance& inst,
                  const PhysicalDeviceSelection& selection,
                  VulkanDevice* out) noexcept;
