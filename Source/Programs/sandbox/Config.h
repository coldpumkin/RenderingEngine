#pragma once

#include <cstdint>

// 바꿔가며 시험하는 값. 컴파일 상수다 - 자주 sweep하게 되면 그때 argv로 받는다.

// CPU가 GPU보다 몇 frame 앞서갈 수 있나. 늘리면 command buffer · semaphore · fence가
// 그만큼 배로 든다.
//
// 실측(120Hz, 삼각형 하나): 1이든 2든 차이 없음. frame 시간의 90%가 acquire(모니터
// 대기)라 CPU가 GPU를 기다리는 상황이 아니었다. GPU가 바빠져야 의미가 생긴다.
constexpr uint32_t kFramesInFlight = 1;

// swapchain image를 몇 장 요청할까. frames-in-flight와 다른 축이다:
//   frames-in-flight  CPU가 몇 frame 앞서나  (fence가 막는다)
//   image 개수        몇 장을 돌리나         (acquire가 막는다)
//
// 요청값일 뿐 - minImageCount 아래로는 못 가고 driver가 더 줄 수도 있다.
//
// 3인 근거는 Khronos 샘플(Vulkan-Samples/performance/swapchain_images)이다. GPU가
// 바쁠 때 2장이면 vsync를 놓쳐 60->30이 되고, 3장이면 안 멈춘다. 우리는 부하가 없어
// 아직 그 차이가 안 보인다.
constexpr uint32_t kDesiredSwapchainImages = 3;

// 우리 image에 그리는 해상도. 창 크기와 무관하다.
//
// surface와 swapchain은 optional extension이라 창 없이도 렌더링이 성립한다. 그래서
// 방향이 "우리 target에 그린 뒤 swapchain으로 내보낸다"이고, 내보내기는 렌더링 단계가
// 아니다. 창과 다른 값으로 놓고 리사이즈해보면 그 독립성이 눈에 보인다.
//
// 고정인 이유: 창을 따라가게 하면 리사이즈마다 frame target을 다시 만드는 경로가
// 하나 더 생긴다. 확대 화질이 문제가 되거나 부하에 따라 해상도를 낮추고 싶어질 때
// 런타임 값이 되고, 그때 재생성 경로가 필요해진다.
constexpr uint32_t kRenderWidth = 1280;
constexpr uint32_t kRenderHeight = 720;

// MSAA sample 수. **요청값이다** - GPU가 color와 depth 양쪽에서 지원하는 것만 쓸 수
// 있어서 ChooseRenderTargetFormats가 이 값 이하로 깎는다. kDesiredSwapchainImages와
// 같은 모양이다.
//
// 왜 render target에만 붙나: swapchain image는 우리가 만드는 것이 아니라 조회해서
// 받는 것이라 sample 수를 정할 자리가 없다. Present pass는 resolve된 1-sample을
// 읽어 그대로 옮기므로 MSAA를 몰라도 된다.
//
// 1로 내리면 resolve attachment가 불법이 된다 (sample 수가 같으면 resolve할 것이
// 없다). 그 분기는 안 만들었다 - 우리 기계에서 안 도는 경로라 검증이 불가능하다.
constexpr uint32_t kDesiredSampleCount = 4;
