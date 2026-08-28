#pragma once

namespace LambdaEngine {

class RHIViewport;

// **기록하는 쪽.** `DynamicRHI`가 만드는 쪽이라면 이쪽은 "무엇을 그리는가"다.
//
// ---------------------------------------------------------------------------
// 인터페이스를 둘로 가른 이유 (언리얼 5.8 실측)
//
//   FDynamicRHI          순수가상 65개. 그중 그리는 함수는 **0개**다.
//                        전부 RHICreate*(팩토리)와 컨텍스트 수명 관리다
//   IRHICommandContext   RHIBeginRenderPass / RHIDrawPrimitive / RHISetViewport ...
//                        그리는 것은 전부 여기 있다
//
// 만드는 일과 기록하는 일은 호출 빈도가 다르다. 팩토리는 가끔 불리고 기록은 드로우콜마다
// 불린다. 한 타입에 두면 "핫패스는 가상함수 금지"를 타입 단위로 말할 수 없게 된다.
//
// 나중에 병렬 기록이 필요해지면 컨텍스트가 스레드마다 하나씩 생긴다. 그때 갈라져 있지
// 않으면 팩토리까지 같이 복제되는 꼴이 된다.
// ---------------------------------------------------------------------------
//
// **한 프레임의 동기화(펜스·세마포어·이미지 인덱스)는 여기 없고 앞으로도 없다.**
// 언리얼도 그렇다 - `IRHICommandContext`에 acquire도 present도 펜스도 없다.
// 그것들은 백엔드가 완전히 숨기고, 상위는 존재조차 모른다.
//
// 수명은 `DynamicRHI`가 갖는다. `BeginDrawingViewport()`가 빌려주고, 다음 프레임에
// 같은 것이 다시 온다 - 그래서 소유권이 아니라 포인터로 돈다.
class RHICommandContext {
public:
    virtual ~RHICommandContext() = default;

    // 이 대상에 그리기 시작한다. 끝나면 반드시 EndRenderPass().
    //
    // **대상을 인자로 받는 이유**: 지금은 뷰포트 하나뿐이라 인자가 없어도 되지만,
    // 오프스크린 렌더 타겟이 생기면 "어디에 그리는가"가 매번 달라진다. 그때 시그니처가
    // 바뀌지 않는 쪽을 택했다. 언리얼도 `RHIBeginRenderPass(FRHIRenderPassInfo&)`로
    // 대상을 받는다.
    //
    // **[임시] 검은색으로 클리어한다.** 언리얼은 클리어 값을 렌더 타겟 리소스가 들고
    // (`ERenderTargetLoadAction::EClear` = "리소스에 지정된 값으로 클리어"), 렌더패스는
    // 클리어할지 말지만 정한다. 우리에겐 아직 텍스처 타입이 없어서 값을 둘 곳이 없다.
    // **텍스처가 생기면 이 결정을 다시 본다.**
    virtual void BeginRenderPass(RHIViewport& target) noexcept = 0;

    virtual void EndRenderPass() noexcept = 0;

    // 다형 타입을 값으로 복사/이동하면 슬라이싱이 난다.
    RHICommandContext(const RHICommandContext&) = delete;
    RHICommandContext& operator=(const RHICommandContext&) = delete;
    RHICommandContext(RHICommandContext&&) = delete;
    RHICommandContext& operator=(RHICommandContext&&) = delete;

protected:
    RHICommandContext() = default;
};

} // namespace LambdaEngine
