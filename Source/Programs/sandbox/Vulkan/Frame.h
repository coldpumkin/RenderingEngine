#pragma once

// Frame - 한 frame을 진행하는 데 필요한 것 한 벌 + 여닫는 연산
// ============================================================================
//
// Commands와 나뉜 이유: 개수의 근거가 다르다.
//   Commands  queue family마다 하나 (pool이 family에 묶인다)
//   Frame     frames-in-flight마다 한 벌
//
// 여닫기가 여기 있는 이유: 그리는 것과 동기화는 바뀌는 이유가 다르다.
//   draw · texture · descriptor를 추가하면     -> RecordFrame
//   frames-in-flight · present mode · queue    -> 여기

#include "Config.h"
#include "Vulkan/Commands.h"
#include "Vulkan/RenderTargets.h"
#include "Vulkan/Window.h"

// 개수는 Config.h의 kFramesInFlight가 정한다. 아래 셋이 같은 신호 하나에 묶여서다.
// 판별은 이렇다: "이 자원을 다시 써도 된다는 걸 무엇이 알려주는가?"
//
//   cmd             pending 상태면 리셋할 수 없다      -> inFlight fence가 알려준다
//   imageAvailable  이전 wait(submit)이 끝나야 재signal -> inFlight fence가 알려준다
//   inFlight        그 자신이 신호다
//
// 셋 다 답이 같은 fence 하나다. 그래서 개수도 같고 한 벌이다.
//
// **renderFinished가 여기 없는 이유도 같은 기준이다.** 그건 present가 기다리는데,
// present에는 완료를 알려주는 것이 없다(vkQueuePresentKHR은 fence를 주지 않는다).
// 유일한 단서가 "acquire가 그 image를 다시 줬다"이고 그건 image index로만 오므로,
// 개수가 image 수가 되어 Swapchain 안에 산다. 그래서 frame 2개와 image 3개가
// 안 맞아도 된다 - 짝지어지지 않는다.
struct Frame {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    // graphics pool에서 나온다. Pool이 죽으면 같이 사라지므로 따로 반납하지 않는다.
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;

    // 이 frame이 그려 넣을 곳. Swapchain이 아니라 우리 image다.
    RenderTargets targets;

    Frame() = default;
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

// Contract: formats는 pipeline이 받는 것과 같아야 한다. main이 한 번 고르고
//           양쪽에 같은 값을 준다.
bool CreateFrame(const VulkanDevice& dev, const Commands& commands,
                 const Descriptors& descriptors,
                 RenderTargetFormats formats, Frame* out) noexcept;

// 이번 frame의 그릴 곳과 내보낼 곳. BeginFrame이 정하고 뒤가 쓴다.
//
// 둘이 갈라진 것이 off-screen rendering의 전부다. 전에는 swapchain image 하나가
// 두 역할을 겸했다 - 거기에 직접 그리고 그대로 내보냈으니 구분할 이유가 없었다.
//
//   draw     우리 image. swapchain이 없어도 성립한다
//   present  swapchain image. 다 그린 결과가 여기로 간다
//
// acquire가 고른 것을 present까지 나른다 - 사이에 RecordFrame이 끼어 있어 지역
// 변수로는 못 건넌다. Present가 요구하는 index는 SwapchainImage가 들고 온다.
struct FrameTarget {
    const RenderTargets* draw = nullptr;
    const SwapchainImage* present = nullptr;
    VkExtent2D presentExtent{};   // 창 크기. draw->extent와 다를 수 있다
};

// 호출자가 무엇을 해야 하는가로 적는다. 내부에서 무슨 일이 있었나가 아니다.
//
// 한때 bool이었는데 false가 세 가지를 뜻하게 됐다 - 최소화(자야 함), swapchain
// 낡음(즉시 재시도), DEVICE_LOST(그만둬야 함). 호출자가 구분할 수 없어 전부
// continue했고, 회복 불가 상태에서 최대 속도로 로그를 뿜는 무한 루프가 됐다.
enum class FrameResult {
    Ready,   // 그린다
    Skip,    // 이번 frame은 없다. 다음 순회에 다시 (swapchain이 낡았다)
    Fatal,   // 회복 불가. 루프를 끝낸다 (DEVICE_LOST, SURFACE_LOST, OOM)
};

// Input:  dev, window, frame
// Output: target (Ready일 때만 유효)
// Effect: 필요하면 swapchain을 다시 만들고, 이전 frame을 기다리고, image를 acquire한다
//
// 최소화는 여기서 안 다룬다. 루프가 WindowHasDrawableSize로 먼저 거른다.
FrameResult BeginFrame(const VulkanDevice& dev,
                       Window* window,
                       const Frame& frame,
                       FrameTarget* out) noexcept;

// Frame의 끝 - 여기서 창과 frame이 갈라진다
// ---------------------------------------------------------------------------
// 한때 EndFrame 하나가 submit과 present를 다 했다. 나눈 근거:
//
//   스펙이 이미 갈라놨다   vkQueueSubmit2     core 1.3
//                          vkQueuePresentKHR  extension VK_KHR_swapchain
//   VkPresentInfoKHR가 받는 것: semaphore · swapchain · image index.
//     Command buffer도 fence도 없다. Submit과 공유하는 건 semaphore 하나뿐.
//   늘어나는 축이 다르다   present는 창 개수, submit은 queue 개수
//
// 시작이 안 갈라지는 이유: acquire가 창의 image index와 frame 소유 semaphore를
// 동시에 만진다. 끝은 그 만남이 풀리는 자리다.

// Input:  dev, frame, signalWhenDone
// Effect: fence를 reset하고 frame.cmd를 graphics queue에 제출한다
//
// Swapchain을 모른다. 창과 이어지는 것은 semaphore 둘(frame.imageAvailable을 기다리고
// signalWhenDone을 신호)뿐이다. Unreal도 같은 모양이다 - viewport가
// Context.AddSignalSemaphore()로 건네주고 context는 출처를 모른다.
bool SubmitFrame(const VulkanDevice& dev,
                 const Frame& frame,
                 VkSemaphore signalWhenDone) noexcept;

// Input:  dev, window, image
// Effect: swapchain image를 화면에 내보낸다. 낡았으면 window에 표시한다.
//
// FrameTarget을 안 받는다 - 한때 받았는데 target.draw를 받아놓고 안 봤다.
// image를 index로 다시 찾지 않고 BeginFrame이 고른 것을 그대로 받는다. Present가
// 요구하는 index는 image가 들고 온다 - 따로 받으면 짝이 어긋나도 컴파일된다.
bool PresentFrame(const VulkanDevice& dev,
                  Window* window,
                  const SwapchainImage& image) noexcept;
