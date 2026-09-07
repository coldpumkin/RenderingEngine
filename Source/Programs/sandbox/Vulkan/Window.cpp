#include "Vulkan/Window.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

// Two types because **glfwInit and glfwTerminate are per process, not per window.**
// A second window would not initialize again, and glfwTerminate destroys **every**
// window there is. Mixed into one function, that goes wrong the moment there are two.

static void OnGlfwError(int code, const char* description) {
    LOG("[glfw] error %d: %s\n", code, description);
}

WindowSystem::~WindowSystem() {
    // **Runs after every window is gone.** glfwTerminate destroys whatever is left, so
    // declaring this first in main -- destroyed last -- is what orders it.
    if (initialized) { glfwTerminate(); }
}

bool InitWindowSystem(WindowSystem* out) noexcept {
    // The error callback goes on before glfwInit, so that glfwInit's own failure has
    // somewhere to report. GLFW documents this as one of the few calls allowed before
    // initialization.
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

// The window and its surface. The swapchain needs a device, which does not exist yet.
//
// **An out parameter, not a return value**, because the address handed to
// glfwSetWindowUserPointer has to be the one the caller will keep. Returned by value,
// what gets registered is the address of a local.
bool OpenWindow(const VulkanInstance& inst,
                int width, int height, const char* title, bool visible,
                Window* out) noexcept {
    out->inst = &inst;

    // GLFW creates an OpenGL context by default. We have no use for one.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // A capture does not look at the window, so it does not open one to look at. The
    // surface, the swapchain and present all work the same either way -- hidden is not
    // minimized, so the extent stays what it was asked for and no frame is skipped.
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);

    out->handle = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (out->handle == nullptr) {
        LOG("[glfw] glfwCreateWindow failed\n");
        return false;
    }
    glfwSetWindowUserPointer(out->handle, out);
    glfwSetFramebufferSizeCallback(out->handle, OnFramebufferResized);

    // glfwCreateWindowSurface exists and is not used. Making the surface ourselves
    // leaves **the window library and Vulkan unaware of each other** -- GLFW never
    // needs to be told what a VkInstance is.
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

// Destroyed inside out: swapchain -> surface -> window.
//
// **The surface has to go before the window**, or it references a dead HWND. The
// swapchain has a destructor of its own, but it has to run **before the surface** --
// and members are destroyed after this body, so it is released here by hand.
Window::~Window() {
    swapchain.reset();

    if (surface != VK_NULL_HANDLE && inst != nullptr) {
        inst->table.vkDestroySurfaceKHR(inst->handle, surface, nullptr);
    }
    if (handle != nullptr) {
        glfwDestroyWindow(handle);
    }
}