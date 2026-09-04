#pragma once

#include "Vulkan/Instance.h"
#include "Vulkan/Swapchain.h"

#include <memory>
#include <vector>

struct GLFWwindow;

// WindowSystem (per process) and Window (per window)
// ============================================================================
//
// Two types because glfwInit and glfwTerminate are per process, not per window: a
// second window would not initialize again, and glfwTerminate destroys every window
// there is.
//
// It has to outlive every window, so it is declared first and destroyed last.
struct WindowSystem {
    bool initialized = false;

    WindowSystem() = default;
    ~WindowSystem();
    WindowSystem(const WindowSystem&) = delete;
    WindowSystem& operator=(const WindowSystem&) = delete;
};

bool InitWindowSystem(WindowSystem* out) noexcept;

// Everything tied to one window
// ============================================================================
//
// The three nest:
//   handle (GLFW)  >  surface (instance level)  >  swapchain (device level)
//
// Destruction follows that nesting. Only the innermost differs in lifetime -- handle
// and surface last as long as the window, while the swapchain is replaced on every
// resize and minimize.
//
// Three reasons the surface is here rather than inside Swapchain:
//   1. it exists before the device -- PickPhysicalDevice needs it to ask whether a
//      queue family can present to this window
//   2. the levels differ: surface is instance, swapchain is device
//   3. it is one to many -- one surface outlives a succession of swapchains
//
// Unreal nests them the other way, with FVulkanSwapChain holding the surface, and then
// needs FVulkanSwapChainRecreateInfo to carry the surface back out every time a
// swapchain dies.
//
// An empty swapchain is a normal state, not an error: it means minimized.
struct Window {
    GLFWwindow* handle = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    // The format belongs to the surface rather than the swapchain -- it is decided by
    // the (GPU, surface) pair. Keeping it here means a pipeline does not have to wait
    // for a swapchain to exist. SelectSurfaceFormat fills it in rather than
    // OpenWindow, because the GPU has to be picked first.
    //
    // **Settled once at startup and left alone.** Recreating the swapchain feeds this
    // same value back in, the way Unreal's FVulkanViewport carries PixelFormat into
    // RecreateSwapchainFromRT.
    //
    // Asking again on every recreate makes "this can change at any time" the premise,
    // and then something has to watch for it in the rendering path. Something did, and
    // it never fired after the first frame.
    //
    // Being wrong about that is not silent: calling vkCreateSwapchainKHR with an
    // unsupported format is a VUID violation and the validation layer reports it. If
    // the format genuinely has to change one day (HDR), that is a request rather than
    // a detection, and the place for it is another call to SelectSurfaceFormat.
    VkSurfaceFormatKHR surfaceFormat{};

    // The other half of what the (GPU, surface) pair answers, and it lives here for
    // the same reason the format does: it is an instance-level fact about this window
    // that outlives any one swapchain.
    //
    // Unlike the format it does change -- every resize -- so it is asked again at the
    // top of each frame rather than settled once. **0x0 means minimized**, which is a
    // state and not an error.
    //
    // Having it here is what lets a render target follow the window without waiting
    // for an image to be acquired: the size is known before the swapchain is remade,
    // not as a by-product of remaking it.
    VkExtent2D surfaceExtent{};

    // The three below are a succession of swapchains rather than anything about a
    // window: the current one, the ones not yet safe to destroy, and whether the
    // current one is stale. They change together on every resize, and nothing above
    // them changes at all after startup.
    //
    // Together here because a surface has exactly one swapchain at a time, so the
    // relation is real -- but it is why ~Window has to release the swapchain by hand
    // before the surface, and why Swapchain.h forward-declares this type.

    // A unique_ptr because a resize replaces the whole thing. By value that needs a
    // move assignment per type; by pointer it is free. And null already means
    // "nowhere to draw right now".
    std::unique_ptr<Swapchain> swapchain;

    // Replaced but not yet releasable. A vector because dragging a window edge
    // overlaps several; empty is the normal state, and it fills for a few frames
    // after a resize.
    std::vector<RetiredSwapchain> retired;

    // One per window. As a global there would be no way to tell which window the
    // resize belonged to.
    bool swapchainOutOfDate = false;


    // Destroying the surface is an instance-level call, so the instance is needed.
    const VulkanInstance* inst = nullptr;

    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};

// The window and its surface. The swapchain needs a device, which does not exist yet.
//
// An out parameter because the address given to glfwSetWindowUserPointer has to be the
// one the caller keeps. Returned by value, what gets registered is a local's address.
bool OpenWindow(const VulkanInstance& inst,
                int width, int height, const char* title,
                Window* out) noexcept;

// SelectSurfaceFormat and QuerySurfaceExtent are declared in Swapchain.h, next to the
// swapchain creation they feed. Both fill a field of this struct: they ask the
// (GPU, surface) pair, which is a surface question with a swapchain answer.
