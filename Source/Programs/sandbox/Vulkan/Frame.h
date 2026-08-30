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
#include "Vulkan/RenderTargets.h"
#include "Vulkan/Window.h"

// 개수는 Config.h의 kFramesInFlight가 정한다.
struct Frame {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 비소유 상태

    // graphics 풀에서 나온다. 풀이 죽으면 같이 사라지므로 따로 반납하지 않는다.
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;

    // **이 프레임이 그려 넣을 곳.** 스왑체인이 아니라 우리 이미지다.
    // 개수의 근거가 Frame과 같아서(동시에 그려지는 프레임 수) 여기 있다.
    RenderTargets targets;
    Frame() = default;
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

// depthFormat: 이 프레임의 렌더 타겟이 쓸 뎁스 포맷. main이 ChooseDepthFormat으로
// 한 번 정해서 파이프라인과 여기에 같이 준다 - 둘이 어긋나면 렌더링이 실패한다.
bool CreateFrame(const VulkanDevice& dev, const Commands& commands,
                 VkFormat depthFormat, Frame* out) noexcept;

// 이번 프레임의 **그릴 곳과 내보낼 곳**. BeginFrame이 정하고 뒤가 쓴다.
//
// **둘이 갈라진 것이 오프스크린의 전부다.** 전에는 스왑체인 이미지 하나가 두 역할을
// 겸했다 - 거기에 직접 그리고 그대로 내보냈으니 구분할 이유가 없었다.
//
//   draw     우리 이미지. 여기에 그린다. 스왑체인이 없어도 성립한다
//   present  스왑체인 이미지. 다 그린 결과를 여기로 복사해 내보낸다
//
// acquire가 고른 것을 present까지 나른다 - 그 사이에 RecordFrame이 끼어 있어
// 지역 변수로는 못 건넌다. (present가 요구하는 인덱스는 SwapchainImage가 들고 온다.)
struct FrameTarget {
    const RenderTargets* draw = nullptr;
    const SwapchainImage* present = nullptr;
    VkExtent2D presentExtent{};   // 창 크기. draw->extent와 다를 수 있다
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

// ---------------------------------------------------------------------------
// 프레임의 끝 - **여기서 창과 프레임이 갈라진다**
//
// 한때 EndFrame 하나가 제출과 present를 다 했다. 나눈 근거:
//
//   스펙이 이미 갈라놨다.  vkQueueSubmit2      코어 1.3
//                          vkQueuePresentKHR   확장 VK_KHR_swapchain
//     스왑체인 확장을 안 켜도 그리고 제출하는 프로그램이 성립한다.
//
//   VkPresentInfoKHR가 받는 것: 세마포어 · 스왑체인 · 이미지 인덱스.
//     **커맨드 버퍼도 펜스도 없다.** 제출과 공유하는 건 세마포어 하나뿐이다.
//
//   늘어나는 축이 다르다.  present는 창 개수(pSwapchains가 배열),
//                          제출은 큐 개수.
//
// **시작은 왜 안 갈라지나**: acquire가 창의 이미지 인덱스와 프레임 소유 세마포어를
// 동시에 만진다. 진짜 만남이라 못 가른다. 끝은 그 만남이 풀리는 자리다.
// ---------------------------------------------------------------------------

// 펜스 리셋 -> 제출. **스왑체인을 모른다.**
//
// 창과 이어지는 것은 세마포어 둘뿐이다: frame.imageAvailable(기다림)과
// signalWhenDone(신호). 스왑체인도 이미지 인덱스도 extent도 안 받는다.
// 언리얼도 같은 모양이다 - 뷰포트가 Context.AddSignalSemaphore()로 건네주고,
// 컨텍스트는 그게 어디서 왔는지 모른다.
bool SubmitFrame(const VulkanDevice& dev,
                 const Frame& frame,
                 VkSemaphore signalWhenDone) noexcept;

// 화면에 내보낸다. 창이 없으면 이 줄만 빼면 된다.
//
// **FrameTarget을 안 받는다.** 한때 받았는데 target.draw(프레임 것)를 받아놓고 안 봤다 -
// "present는 프레임을 모른다"가 말로만 참이고 시그니처에선 거짓이었다.
//
// image를 인덱스로 다시 찾지 않고 **BeginFrame이 고른 것을 그대로 받는다.** 지금은
// 프레임 도중에 스왑체인이 안 바뀌어서 둘이 같지만, 바뀌게 되면 인덱스는 다른 이미지를
// 가리키고 참조는 안 그렇다. FrameTarget이 있는 이유가 그 "이미 고른 것"을 나르는 것이다.
//
// 인덱스는 image가 들고 온다 - 둘이 짝이 맞아야 하는데 따로 받으면 어긋나도 컴파일된다.
bool PresentFrame(const VulkanDevice& dev,
                  Window* window,
                  const SwapchainImage& image) noexcept;
