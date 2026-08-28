#pragma once

#include "DynamicRHI.h"

#include <memory>

// GLFW의 창 타입. 전방 선언만 한다.
//
// 포인터 멤버에는 타입의 정의가 필요 없다. 덕분에 이 헤더는 glfw3.h를 include하지 않고,
// Engine을 쓰는 쪽은 GLFW를 모른 채로 남는다.
struct GLFWwindow;

namespace LambdaEngine {

// 엔진 초기화와 소유를 담당한다. 루프는 여기 없다.
//
// 초기화(창 시스템 -> 창 -> RHI)는 어떤 프로그램이든 반복되므로 여기 모은다.
// 루프는 프로그램마다 다르고, sandbox는 프레임 안에서 실험하는 작업대라 루프에
// 자유롭게 끼어들 수 있어야 한다. Run()은 프레임 순서를 엔진이 보장해야 할 때 생긴다. (D50)
//
// 창 크기 변화는 **입력**이고 입력은 여기서 안다. 그래서 창 콜백을 받아 RHI에 통보한다.
class Engine {
public:
    // 창 시스템 초기화 + 창 생성 + RHI 생성까지. 실패하면 nullptr.
    // 생성되면 이미 전부 사용 가능하다 (Init() 없음).
    static std::unique_ptr<Engine> Create(int width, int height, const char* title) noexcept;

    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    // 창 이벤트를 처리한다. 창이 닫혔으면 false.
    bool PumpEvents() noexcept;

    // non-owning 참조. 수명은 계속 Engine에 있다.
    DynamicRHI& RHI() noexcept { return *rhi_; }

    // 뷰포트를 만들려면 필요하다. **뷰포트는 Engine이 만들지 않는다** -
    // Engine은 뷰포트 없이도 유효하므로 생성 조건이 아니고, 그리려는 쪽이 만든다.
    NativeWindowHandle WindowHandle() const noexcept;

    // 창 크기가 바뀌었다는 통보를 **가져간다**(한 번 읽으면 지워진다).
    //
    // Engine이 뷰포트에 직접 알리지 않는 이유: 뷰포트를 소유하지 않기 때문이다.
    // **입력은 여기서 받고**(GLFW 콜백은 창에 걸린다) **적용은 뷰포트를 가진 쪽이 한다.**
    bool ConsumeResized() noexcept;

    // 창 콜백이 부른다. 표시만 한다.
    void MarkResized() noexcept;

private:
    Engine(GLFWwindow* window, std::unique_ptr<DynamicRHI> rhi) noexcept;

    GLFWwindow* window_ = nullptr;      // 소유한다. 소멸자에서 직접 파괴
    std::unique_ptr<DynamicRHI> rhi_;

    // 창 콜백이 세우고 ConsumeResized()가 가져간다.
    bool resized_ = false;
};

} // namespace LambdaEngine
