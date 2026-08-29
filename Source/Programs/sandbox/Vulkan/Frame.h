#pragma once

// **한 프레임을 진행하는 데 필요한 것 한 벌** + 그 프레임을 여닫는 연산.
//
// ---------------------------------------------------------------------------
// **왜 Commands와 나뉘어 있나**: 개수의 근거가 다르다.
//   Commands   큐 패밀리마다 하나 (풀이 패밀리에 묶인다)
//   Frame      frames-in-flight마다 한 벌
//
// **왜 여기에 BeginFrame/EndFrame이 있나**: 루프에서 그리는 것과 동기화는
// **바뀌는 이유가 다르다.**
//   드로우·텍스처·디스크립터를 추가하면  -> RecordFrame만 바뀐다
//   frames-in-flight·present 모드·큐를 바꾸면 -> 여기만 바뀐다
// 한 루프에 섞여 있으면 드로우 하나 추가하려고 동기화 코드를 스크롤해야 한다.
//
// 언리얼도 같은 자리에서 자른다 (FDynamicRHI의 프레임 여닫기 vs IRHICommandContext의 기록).
// ---------------------------------------------------------------------------

#include "Config.h"
#include "Vulkan/Commands.h"
#include "Vulkan/Window.h"

// 개수는 Config.h의 kFramesInFlight가 정한다.
struct Frame {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 비소유 상태

    // graphics 풀에서 나온다. 풀이 죽으면 같이 사라지므로 따로 반납하지 않는다.
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    Frame() = default;
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

bool CreateFrame(const VulkanDevice& dev, const Commands& commands, Frame* out) noexcept;

// 이번 프레임에 그릴 대상. **BeginFrame이 정하고 RecordFrame과 EndFrame이 쓴다.**
//
// imageIndex를 들고 있는 이유: acquire가 준 값을 present가 다시 써야 하는데,
// 그 사이에 RecordFrame이 끼어 있어 지역 변수로는 건널 수 없다.
struct FrameTarget {
    const SwapchainImage* image = nullptr;
    VkExtent2D extent{};
    uint32_t imageIndex = 0;
};

// **호출자가 무엇을 해야 하는가**로 적는다. 내부에서 무슨 일이 있었나가 아니다.
//
// 한때 bool이었는데 false가 세 가지를 뜻하게 됐다 - 최소화(자야 함), 스왑체인
// 낡음(즉시 재시도), DEVICE_LOST(그만둬야 함). 호출자가 구분할 수 없으니 전부
// continue했고, 그래서 회복 불가 상태에서 **최대 속도로 로그를 뿜는 무한 루프**가 됐다.
enum class FrameResult {
    Ready,   // 그린다
    Skip,    // 이번 프레임은 없다. 다음 순회에 다시 (스왑체인이 낡았다)
    Fatal,   // 회복 불가. 루프를 끝낸다 (DEVICE_LOST, SURFACE_LOST, 메모리 부족)
};

// 프레임을 연다: 그릴 곳 확보 -> 이전 프레임 대기 -> 이미지 확보.
//
// **최소화는 여기서 안 다룬다.** 루프가 WindowHasDrawableSize로 먼저 거른다.
FrameResult BeginFrame(const VulkanDevice& dev,
                       Window* window,
                       const Frame& frame,
                       FrameTarget* out) noexcept;

// 프레임을 닫는다: 펜스 리셋 -> 제출 -> 화면에 표시.
// **BeginFrame이 Ready를 준 프레임에만 부른다.**
//
// 결과가 실제로 둘뿐이라(계속 / 그만) bool이다. Skip이 나올 자리가 없으므로
// 억지로 FrameResult를 쓰지 않는다.
bool EndFrame(const VulkanDevice& dev,
              Window* window,
              const Frame& frame,
              const FrameTarget& target) noexcept;
