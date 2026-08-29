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

// 프레임을 연다: 그릴 곳 확보 -> 이전 프레임 대기 -> 이미지 확보.
//
// **false는 실패가 아니라 "이번 프레임은 없다"**이다 (최소화 중이거나 스왑체인이 낡음).
// 호출자는 continue한다.
bool BeginFrame(const VulkanDevice& dev,
                Window* window,
                const Frame& frame,
                FrameTarget* out) noexcept;

// 프레임을 닫는다: 펜스 리셋 -> 제출 -> 화면에 표시.
// **BeginFrame이 false를 준 프레임에는 부르지 않는다.**
//
// BeginFrame과 같이 bool이다. 제출은 실패할 수 있고, 실패하면 이 프레임에 그린 것은
// 화면에 안 나온다 - 호출자가 알아야 하는 사실이다.
bool EndFrame(const VulkanDevice& dev,
              Window* window,
              const Frame& frame,
              const FrameTarget& target) noexcept;
