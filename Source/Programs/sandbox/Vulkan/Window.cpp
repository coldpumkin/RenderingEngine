#include "Vulkan/Window.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

// ============================================================================
// 2. 창 시스템 (프로세스 하나당) + 창 (창마다)
// ============================================================================
//
// 둘을 나눈 이유: **glfwInit/glfwTerminate는 창이 아니라 프로세스 단위다.**
// 창이 둘이 돼도 초기화는 한 번이고, glfwTerminate는 **모든** 창을 부순다.
// 한 함수에 섞여 있으면 창이 둘 될 때 바로 어긋난다.

static void OnGlfwError(int code, const char* description) {
    LOG("[glfw] error %d: %s\n", code, description);
}

WindowSystem::~WindowSystem() {
    // **모든 창이 죽은 뒤에 불려야 한다.** glfwTerminate는 남은 창을 전부 부순다.
    // main()에서 제일 먼저 선언하면(= 제일 나중에 파괴) 그 순서가 보장된다.
    if (initialized) { glfwTerminate(); }
}

bool InitWindowSystem(WindowSystem* out) noexcept {
    // 에러 콜백을 glfwInit보다 먼저 건다. glfwInit 자체의 실패 이유도 받으려면 그래야 한다
    // (GLFW 문서가 명시하는, 초기화 전에 부를 수 있는 예외 함수).
    glfwSetErrorCallback(OnGlfwError);
    if (glfwInit() != GLFW_TRUE) {
        LOG("[glfw] glfwInit failed\n");
        return false;
    }
    out->initialized = true;
    return true;
}

// ---------------------------------------------------------------------------
static void OnFramebufferResized(GLFWwindow* handle, int /*w*/, int /*h*/) {
    auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (window != nullptr) {
        window->swapchainOutOfDate = true;
    }
}

// 창 + 서피스까지 만든다. 스왑체인은 디바이스가 생긴 뒤라 여기서 못 만든다.
//
// **out으로 받는 이유**: glfwSetWindowUserPointer에 넣을 주소가 **호출자가 들고 있을
// 최종 주소**여야 한다. 값으로 반환하면 함수 안의 임시 객체 주소를 넣게 된다.
bool OpenWindow(const VulkanInstance& inst,
                int width, int height, const char* title,
                Window* out) noexcept {
    out->inst = &inst;

    // GLFW는 기본적으로 OpenGL 컨텍스트를 같이 만든다. Vulkan을 쓰므로 끈다.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    out->handle = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (out->handle == nullptr) {
        LOG("[glfw] glfwCreateWindow failed\n");
        return false;
    }
    glfwSetWindowUserPointer(out->handle, out);
    glfwSetFramebufferSizeCallback(out->handle, OnFramebufferResized);

    // glfwCreateWindowSurface()도 있지만 쓰지 않는다. 직접 만들면 **창 라이브러리와
    // Vulkan이 서로를 모르는 상태로 남는다** - GLFW가 VkInstance를 알 필요가 없다.
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = glfwGetWin32Window(out->handle);

    if (inst.table.vkCreateWin32SurfaceKHR(inst.handle, &info, nullptr, &out->surface)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateWin32SurfaceKHR failed\n");
        glfwDestroyWindow(out->handle);
        out->handle = nullptr;
        return false;
    }
    return true;
}

bool WindowHasDrawableSize(const Window& window) noexcept {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window.handle, &width, &height);
    return width > 0 && height > 0;
}

// 중첩의 역순으로 부순다: 스왑체인 -> 서피스 -> 창.
//
// **서피스가 창보다 먼저 죽어야 한다** - 죽은 HWND를 참조하게 된다.
// 스왑체인은 자기 소멸자가 알아서 처리하지만, **서피스보다 먼저** 죽어야 하므로
// 여기서 명시적으로 먼저 놓는다 (멤버 파괴는 이 본문 뒤에 일어난다).
Window::~Window() {
    swapchain.reset();

    if (surface != VK_NULL_HANDLE && inst != nullptr) {
        inst->table.vkDestroySurfaceKHR(inst->handle, surface, nullptr);
    }
    if (handle != nullptr) {
        glfwDestroyWindow(handle);
    }
}