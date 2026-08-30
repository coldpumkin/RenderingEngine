#pragma once

#include "Vulkan/Instance.h"

#include <vma/vk_mem_alloc.h>

// 3. 물리 디바이스 고르기 + 큐 패밀리 고르기
// ============================================================================
//
// **큐 패밀리가 뭔가**
//
// GPU는 명령을 "큐"에 넣어야 실행한다. 그런데 모든 큐가 모든 일을 하지는 않는다.
// GPU는 큐를 **패밀리**로 묶어서 "이 그룹은 그래픽스+컴퓨트+전송을 다 하고, 저 그룹은
// 전송만 전담한다"는 식으로 알려준다. 전송 전담 패밀리는 보통 별도 DMA 엔진이라
// 그래픽스와 **물리적으로 병렬로** 돈다 - 그게 패밀리를 나눠 놓은 이유다.
//
// 스펙상 GRAPHICS나 COMPUTE 비트가 있으면 전송은 **암묵적으로 지원된다**
// (TRANSFER 비트가 안 켜져 있어도 된다). 그래서 "전송 가능한가"를 물으려고
// TRANSFER 비트를 보면 안 되고, "**전송만** 하는 전용 패밀리인가"를 물을 때 본다.
//
// 데스크톱 GPU의 전형적인 모습:
//   family 0 : GRAPHICS | COMPUTE | TRANSFER   범용 큐
//   family 1 : COMPUTE  | TRANSFER             async compute
//   family 2 : TRANSFER                        DMA 엔진
struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;   // **필수.** present도 여기서 한다
    uint32_t compute  = UINT32_MAX;   // 없을 수 있다
    uint32_t transfer = UINT32_MAX;   // 없을 수 있다

    bool HasCompute()  const noexcept { return compute  != UINT32_MAX; }
    bool HasTransfer() const noexcept { return transfer != UINT32_MAX; }
};

// **전용이 아니면 안 만든다.**
//
// 컴퓨트/전송 큐를 따로 두는 목적은 그래픽스와 **동시에** 도는 것이다. 같은 패밀리로
// 대체하면 그 이득은 없으면서, 큐가 갈리는 순간 생기는 비용은 그대로 낸다:
// 큐 사이 동기화(세마포어)와 **큐 패밀리 소유권 이전**(release/acquire 배리어 한 쌍).
//
// 그래서 전용 패밀리가 없으면 UINT32_MAX로 두고, 그 일은 그래픽스 큐가 한다.
// 언리얼도 같다 - 전용을 못 찾으면 Queues[AsyncCompute]를 nullptr로 둔다
// (VulkanDevice.cpp: "If we didn't find a dedicated Queue, leave it null").
//
// 그래픽스 하나만 못 찾으면 실패다. 화면에 못 그리면 이 엔진은 할 일이 없다.
struct PhysicalDeviceSelection {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    QueueFamilies families;
};

// GPU 고르기. 자격을 통과한 것 중 외장을 선호한다.
// 실패하면 gpu가 VK_NULL_HANDLE인 채로 돌아온다.
PhysicalDeviceSelection PickPhysicalDevice(const VulkanInstance& inst,
                                           VkSurfaceKHR surface) noexcept;

// 4. 논리 디바이스 + 함수 테이블 + 큐들
// ============================================================================

// 만든 큐 핸들들. **compute/transfer는 VK_NULL_HANDLE일 수 있다** -
// 전용 패밀리가 없다는 뜻이고, 그때 그 일은 graphics가 한다.
struct Queues {
    VkQueue graphics = VK_NULL_HANDLE;
    VkQueue compute  = VK_NULL_HANDLE;
    VkQueue transfer = VK_NULL_HANDLE;

    // **present는 4번째 큐가 아니라 역할이다.** 위 셋 중 하나를 가리키는 별칭이고,
    // 기본은 그래픽스다. 언리얼도 같다:
    //   FVulkanQueue* PresentQueue = nullptr;  // points to an existing queue
    //   (VulkanDevice.h:718, EVulkanQueueType은 Graphics/AsyncCompute/Transfer 셋뿐)
    //
    // 그래픽스가 present를 못 하는 하드웨어는 지원하지 않는다 - 언리얼도 그 경우
    // 메시지박스를 띄우고 종료한다(VulkanSwapChain.cpp:886 SetupPresentQueue).
    // 지원하려면 스왑체인을 CONCURRENT로 바꾸거나 소유권 이전을 넣어야 하고,
    // Win32 단일 GPU에서는 일어나지 않는 경우다.
    //
    // **나중에 볼 것**: AMD에서는 컴퓨트 큐로 present하는 빠른 경로가 있다.
    // 언리얼이 vendor를 AMD로 한정해 cvar 뒤에 두고 있다:
    //   bPresentOnComputeQueue = (VendorId == EGpuVendorId::Amd);
    // 제출 구조가 바뀌므로 **지연을 실제로 잴 수 있을 때** 검토한다.
    VkQueue present = VK_NULL_HANDLE;

    // 없으면 그래픽스로 떨어진다. 호출부가 매번 분기하지 않게.
    VkQueue ComputeOrGraphics()  const noexcept { return compute  ? compute  : graphics; }
    VkQueue TransferOrGraphics() const noexcept { return transfer ? transfer : graphics; }
};

// 디바이스 층. **다섯이 한 몸이다.**
//
// 근거: 디바이스가 생긴 뒤로 gpu · families · queues가 device 없이 쓰이는 곳이
// 하나도 없다. 반대로 device를 쓰는 곳은 거의 다 table도 같이 쓴다.
// 그래서 PhysicalDeviceSelection이 여기로 흡수된다 - 선택 결과는 디바이스의 정체다.
//
// 인스턴스와 같은 이유로 table과 handle이 같이 있고, 같은 이유로 필요 범위는 다르다:
// vkCmd*는 커맨드 버퍼로 디스패치하므로 기록 함수는 table만 있으면 되고 handle은 필요 없다.
//
// **아직 소멸자가 없다.** 인스턴스와 같다 - 정리 순서가 아플 때 RAII로 옮긴다.
struct VulkanDevice {
    VolkDeviceTable table{};
    VkDevice handle = VK_NULL_HANDLE;

    // 선택 결과가 여기로 흡수됐다. 파괴할 것이 없는 값들이라 소유가 아니다.
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    QueueFamilies families;

    // vkGetDeviceQueue는 **조회**다. vkCreateDevice가 이미 만들었고 파괴 함수도 없다.
    Queues queues;

    // 이 GPU의 메모리 타입 목록. **여기 두는 이유**(기준 A ③):
    //   생성 시 확정 · 사는 동안 불변 · 디바이스가 죽으면 의미 상실 - 셋 다 참이다.
    //
    // 조회 함수(vkGetPhysicalDeviceMemoryProperties)는 **인스턴스 레벨**이라 나중에
    // 다시 물으려면 인스턴스가 필요하다. 버퍼를 만들 때마다 인스턴스를 끌고 다니는
    // 대신, 안 변하는 값이니 여기 한 번 담아둔다.
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    // **뎁스 포맷은 여기 없다.** 한때 있었는데, 후보 목록과 그 우선순위는 우리
    // 렌더 타겟의 정책이지 GPU의 성질이 아니다 - GPU는 "지원하나"에만 답한다.
    // memoryProperties가 GPU가 말한 것이라면 그건 그중에서 우리가 고른 것이다.
    // 지금은 RenderTargets.h의 ChooseDepthFormat에 있다.

    // GPU 메모리 할당자. **디바이스가 만들고 디바이스와 함께 죽는다.**
    //
    // 한때 여기 있던 FindMemoryType + vkAllocateMemory + vkBindBufferMemory 60여 줄을
    // 이것이 대신한다. 무엇을 대신하는지는 CreateBuffer 주석 참고.
    VmaAllocator allocator = VK_NULL_HANDLE;

    // 인스턴스와 같다 - vkDeviceWaitIdle과 vkDestroyDevice가 둘 다 디바이스 레벨이라
    // 자기 테이블로 자기를 지운다.
    VulkanDevice() = default;
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
};

// 논리 디바이스 + 함수 테이블 + 큐들.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// 논리 디바이스 + 함수 테이블 + 큐들.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// **inst를 받는 이유**: vkCreateDevice는 **인스턴스 레벨 함수**다. 만드는 함수와
// 파괴하는 함수(vkDestroyDevice, 디바이스 레벨)의 층이 다르다는 Vulkan API의 비대칭이고,
// 그래서 "이 클래스가 무슨 레벨이냐"가 아니라 "이 호출이 무슨 레벨이냐"로 봐야 한다.
bool CreateDevice(const VulkanInstance& inst,
                  const PhysicalDeviceSelection& selection,
                  VulkanDevice* out) noexcept;