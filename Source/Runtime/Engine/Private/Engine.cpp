#include "Engine.h"

#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <cstdio>
#include <utility>

namespace LambdaEngine {
namespace {

void OnGlfwError(int code, const char* description) {
    std::fprintf(stderr, "[Engine] GLFW error %d: %s\n", code, description);
}

// 창 크기가 바뀌면 RHI에 알린다. 크기 값은 넘기지 않는다 - 실제 크기는 서피스에게
// 물어야 정확하고(DPI 스케일링), 여기 오는 값과 다를 수 있다.
void OnFramebufferResized(GLFWwindow* window, int /*width*/, int /*height*/) {
    auto* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
    if (engine != nullptr) {
        // 표시만 한다. **뷰포트를 여기서 부르지 않는 이유**: Engine은 뷰포트를 소유하지
        // 않으므로 어느 뷰포트인지 알 수 없다. 적용은 가진 쪽이 한다.
        engine->MarkResized();
    }
}

} // namespace

std::unique_ptr<Engine> Engine::Create(int width, int height, const char* title) noexcept {
    // 에러 콜백을 glfwInit()보다 먼저 건다. glfwInit() 자체의 실패 이유도 받으려면
    // 그래야 한다 (GLFW 문서가 명시하는, 초기화 전에 부를 수 있는 예외 함수).
    glfwSetErrorCallback(OnGlfwError);

    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "[Engine] glfwInit failed\n");
        return nullptr;
    }

    // GLFW는 기본적으로 OpenGL 컨텍스트를 같이 만든다. Vulkan을 쓰므로 끈다.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow* window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "[Engine] glfwCreateWindow failed\n");
        glfwTerminate();
        return nullptr;
    }

    // RHI는 창을 모른다 - GPU backend는 창이 하나도 없어도 성립한다.
    auto rhi = CreateRHI();
    if (rhi == nullptr) {
        // 실패 이유는 RHI가 이미 기록했다. 여기서는 우리가 만든 것만 되돌린다.
        glfwDestroyWindow(window);
        glfwTerminate();
        return nullptr;
    }

    std::fprintf(stderr, "[Engine] ready (%dx%d)\n", width, height);
    return std::unique_ptr<Engine>(new Engine(window, std::move(rhi)));
}

Engine::Engine(GLFWwindow* window, std::unique_ptr<DynamicRHI> rhi) noexcept
    : window_(window), rhi_(std::move(rhi)) {
    // 콜백이 this를 찾을 수 있게 창에 매달아둔다. 생성자에서 하는 이유는
    // 이 시점부터 rhi_가 유효하기 때문이다.
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, OnFramebufferResized);
}

Engine::~Engine() {
    // 소멸자 본문이 멤버 파괴보다 먼저 실행된다. 여기서 창을 그냥 파괴하면 아직
    // 살아있는 rhi_(그 안의 VkSurfaceKHR)가 죽은 창을 참조한다.
    //
    // 콜백부터 끊는다. reset한 뒤에도 이벤트가 들어오면 죽은 뷰포트를 부른다.
    glfwSetFramebufferSizeCallback(window_, nullptr);
    glfwSetWindowUserPointer(window_, nullptr);

    // **뷰포트는 여기 없다.** 그리려는 쪽이 소유하고, 그쪽이 Engine보다 먼저 죽는다.
    rhi_.reset();
    glfwDestroyWindow(window_);
    glfwTerminate();

    std::fprintf(stderr, "[Engine] shut down\n");
}

NativeWindowHandle Engine::WindowHandle() const noexcept {
    // glfwCreateWindowSurface()도 있지만 쓰지 않는다 - 그걸 쓰면 창 라이브러리가
    // VkInstance를 알아야 하고 창과 RHI가 서로 얽힌다. 핸들만 넘기면 서로 모른 채 남는다.
    NativeWindowHandle handle{};
    handle.instance = GetModuleHandleW(nullptr);
    handle.window = glfwGetWin32Window(window_);
    return handle;
}

void Engine::MarkResized() noexcept {
    resized_ = true;
}

bool Engine::ConsumeResized() noexcept {
    const bool was = resized_;
    resized_ = false;
    return was;
}

bool Engine::PumpEvents() noexcept {
    glfwPollEvents();
    return glfwWindowShouldClose(window_) == 0;
}

} // namespace LambdaEngine
