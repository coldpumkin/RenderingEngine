#pragma once

#include "NativeWindowHandle.h"
#include "RHICommandContext.h"
#include "RHIViewport.h"

#include <memory>

namespace LambdaEngine {

// 상위 코드가 아는 유일한 타입. 구현체(VulkanRHI)는 여기서 보이지 않는다.
//
// **수명이 둘이고 이 타입은 그중 GPU backend 쪽이다.**
//
//   GPU backend        창이 하나도 없어도 존재하고, 창이 다 닫혀도 산다
//                      (인스턴스 · GPU 선택 · 디바이스 · 큐 · 앞으로 올 할당자/캐시)
//   Window/presentation 특정 창이 사는 동안만 의미가 있다
//                      (서피스 · 스왑체인 · 그 이미지들)
//
// 그래서 RHI의 생성 조건에 창이 없다 - "이 기계에서 GPU 작업을 할 수 있는가"가 전부다.
//
// 정체는 초기화 코드가 아니라 팩토리다. 백엔드를 갈아끼울 때 달라지는 것은
// "무엇을 만드느냐"이므로, 백엔드 경계에 서는 타입은 팩토리가 된다.
// CreateBuffer/CreateTexture가 여기 들어온다.
//
// **그리는 함수는 여기 없다.** 그건 `RHICommandContext`가 한다. 언리얼도 같다 -
// `FDynamicRHI`의 순수가상 65개 중 드로우는 0개이고 전부 `RHICreate*`다.
// 여기 남는 프레임 관련 함수는 **프레임을 여닫는 것 둘**뿐이다.
//
// Init()/Shutdown()이 없는 것은 의도다. 분리를 없애면 "Init 두 번", "Shutdown 누락",
// "순서 뒤집기"가 표현조차 불가능해진다. (D14)
class DynamicRHI {
public:
    virtual ~DynamicRHI() = default;

    // 이 창에 그리는 뷰포트를 만든다. 실패하면 nullptr.
    //
    // **RHI가 만들고 소유권을 넘긴다.** 뷰포트는 RHI가 아니라 창의 수명을 따르므로
    // RHI 안에 두면 불변식이 섞인다 (D95). 창이 둘이면 뷰포트도 둘이다.
    virtual std::unique_ptr<RHIViewport> CreateViewport(NativeWindowHandle window) noexcept = 0;

    // 이 뷰포트가 붙은 창의 크기가 바뀌었다는 통보.
    //
    // **크기 값을 받지 않는다** - 실제 크기는 서피스에게 물어야 정확하고(DPI 스케일링)
    // 창이 알려주는 값과 다를 수 있다. 안 쓸 인자를 받지 않는다.
    //
    // 이 통보가 필요한 이유: 창 크기는 입력이고 입력은 상위가 안다. present 결과
    // (VK_SUBOPTIMAL_KHR)로 추측할 수도 있지만 그건 한 프레임 늦고 드라이버마다 다르다.
    //
    // 통보만 하고 실제 재생성은 다음 프레임 시작에 한다 - 창 콜백은 아무 때나 오고
    // 그 시점에 GPU가 프레임 중일 수 있다.
    virtual void ResizeViewport(RHIViewport& viewport) noexcept = 0;

    // ------------------------------------------------------------------
    // 프레임을 여닫는다. 이 둘 사이에서만 기록할 수 있다.
    // ------------------------------------------------------------------

    // 이 뷰포트에 그릴 준비를 하고 기록기를 빌려준다.
    //
    // **지금 그릴 수 없으면 nullptr**이고 그건 실패가 아니다 - 창이 최소화되면 스왑체인을
    // 만들 수 없고, 그때는 이번 프레임을 건너뛰는 것이 정상이다. 호출자는 continue한다.
    //
    // 이 안에서 벌어지는 일(스왑체인 재생성 · 이전 프레임 대기 · 이미지 확보 · 기록 시작)은
    // **상위가 하나도 몰라야 한다.** 펜스도 세마포어도 이미지 인덱스도 여기 인자로 오지
    // 않는다. 언리얼의 RHI 인터페이스에도 그것들은 없다.
    //
    // 소유권을 넘기지 않는다. 컨텍스트는 매 프레임 다시 만들어지는 것이 아니라 재사용된다.
    virtual RHICommandContext* BeginDrawingViewport(RHIViewport& viewport) noexcept = 0;

    // 기록을 닫고, 제출하고, 화면에 내보낸다.
    //
    // 셋을 한 함수로 둔 이유: 셋 다 상위가 정할 것이 없다. 상위가 아는 것은 "이 프레임은
    // 끝났다"뿐이고, 순서와 동기화는 백엔드의 몫이다. 언리얼도 `RHIEndDrawingViewport`
    // 하나가 이 자리를 맡는다.
    //
    // `BeginDrawingViewport()`가 nullptr을 준 프레임에는 부르지 않는다.
    virtual void EndDrawingViewport(RHIViewport& viewport) noexcept = 0;

    // 다형 타입을 값으로 복사/이동하면 슬라이싱이 난다.
    DynamicRHI(const DynamicRHI&) = delete;
    DynamicRHI& operator=(const DynamicRHI&) = delete;
    DynamicRHI(DynamicRHI&&) = delete;
    DynamicRHI& operator=(DynamicRHI&&) = delete;

protected:
    DynamicRHI() = default;
};

// **창을 받지 않는다.** 생성 조건은 "이 기계에서 우리 엔진이 GPU 작업을 할 수 있는가"뿐이다:
// 로더가 있고, 1.3 이상이고, 인스턴스를 만들 수 있고, 요구를 만족하는 GPU가 있고,
// 그 GPU에 graphics + 플랫폼 present 큐 패밀리가 있는가.
//
// 한때 창을 받았다. 이유는 하나였다 - 디바이스를 고를 때 "이 서피스에 present 되는가"를
// 물었기 때문이다. 그 질문을 플랫폼 수준으로 바꾸자(vkGetPhysicalDeviceWin32PresentationSupportKHR)
// 창을 받을 이유가 사라졌다.
//
// 소유권을 반환한다 (Core Guidelines I.11/R.3).
//
// 실패하면 nullptr. 로더나 GPU가 없는 것은 환경 문제라 호출자가 정상 종료할 수 있어야
// 한다. 프로그래머 오류는 안에서 fail-fast한다. (D16)
std::unique_ptr<DynamicRHI> CreateRHI() noexcept;

} // namespace LambdaEngine
