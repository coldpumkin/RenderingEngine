#pragma once

namespace LambdaEngine {

// **창 하나에 붙어서 화면에 내보내는 것.** RHI가 만들어 돌려주는 첫 리소스다.
//
// ---------------------------------------------------------------------------
// "뷰포트"라는 말은 두 가지를 가리킨다. 이 타입은 그중 첫 번째다.
//
//   ① **present 대상**  창에 붙어 백버퍼를 돌리는 것. 스왑체인을 안에 가진다.
//                       수명은 창에 묶인다. 언리얼의 FRHIViewport.
//
//   ② **드로우 상태**   렌더 타겟 안에서 그릴 사각 영역 + 깊이 범위(VkViewport).
//                       매 드로우 바뀔 수 있는 커맨드 상태다. 언리얼의 RHISetViewport().
//
// **②는 여기 없고 앞으로도 여기 없다.** "화면의 어느 영역에 그리는가"는 상위가 정하는
// 입력이고, 그리는 쪽은 그게 뭔지 모르는 채로 그려야 한다. ②는 커맨드 리스트가 생길 때
// 그쪽 인자로 들어온다. (D82의 원래 취지가 이것이었다)
// ---------------------------------------------------------------------------
//
// 왜 리소스인가 (D95): 수명이 GPU backend와 다르다. RHI(인스턴스·디바이스·큐)는 창이
// 하나도 없어도 존재하고 창이 다 닫혀도 살지만, 이것은 **특정 창이 사는 동안만** 의미가
// 있다. 불변식이 다른 둘을 한 객체에 담지 않는다.
//
// 안을 열지 않는다. 서피스도 스왑체인도 이미지도 상위에서 보이지 않는다 -
// 그것들은 백엔드마다 다르고, 상위가 아는 것은 "이 창에 그린다"뿐이어야 한다.
//
// **가상 함수가 소멸자 하나뿐인 것은 의도다.** 리소스는 *무엇인가*를 나타내지
// *무엇을 하는가*가 아니다. 뷰포트에 대한 동작은 전부 `DynamicRHI`가 이 타입을 받아서
// 한다 - 그리는 데 필요한 것(디바이스·큐·커맨드 풀)이 거기 있기 때문이다.
// 그래서 "뷰포트로 무엇을 할 수 있나"는 `DynamicRHI.h` 한 곳만 보면 된다.
//
// 언리얼도 같다: `FRHIViewport`의 가상 함수는 전부 플러그인용 `GetNative*`이고,
// 핵심 동작(`RHIResizeViewport`, `RHIGetViewportBackBuffer`, `RHIBeginDrawingViewport`)은
// 전부 `FDynamicRHI` 쪽에서 뷰포트를 **인자로** 받는다.
//
// 소멸자만 가상인 이유: 소유권이 `unique_ptr<RHIViewport>`로 넘어가므로 상위가 구현체를
// 파괴할 수 있어야 한다. 그것 말고는 다형성이 필요한 자리가 없다.
class RHIViewport {
public:
    virtual ~RHIViewport() = default;

    // 다형 타입을 값으로 복사/이동하면 슬라이싱이 난다.
    RHIViewport(const RHIViewport&) = delete;
    RHIViewport& operator=(const RHIViewport&) = delete;
    RHIViewport(RHIViewport&&) = delete;
    RHIViewport& operator=(RHIViewport&&) = delete;

protected:
    RHIViewport() = default;
};

} // namespace LambdaEngine
