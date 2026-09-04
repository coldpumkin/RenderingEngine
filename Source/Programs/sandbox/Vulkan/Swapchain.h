#pragma once

#include "Vulkan/Attachments.h"
#include "Vulkan/Texture.h"

#include <memory>
#include <vector>

// Swapchain - the one thing here with a lifetime shorter than the program
// ============================================================================
//
// Everything else is created once and kept. The swapchain is remade whenever the
// window changes size, which makes it the only piece that moves on its own.

// Forward declared because EnsureSwapchain takes one. Window holds a Swapchain, so
// Window.h includes this file; going back the other way, the name is enough.
struct Window;

// An image something draws into, so it is a Texture -- the same type our own
// attachments are, and its desc says what format it is. The one difference is that
// there is no allocation: the image was queried, and only the view is ours.
//
// Owned by the swapchain and never handed out. What leaves is a FrameTarget naming one
// of these as this frame's destination, and the index lives there too -- a copy of the
// array position kept here would be a copy that can go stale.
struct SwapchainImage {
    Texture texture;
    VkSemaphore renderFinished = VK_NULL_HANDLE;  // one per image; why is in the .cpp
};

struct Swapchain {
    const struct VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<SwapchainImage> images;

    Swapchain() = default;
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
};

// On failure, or on "not right now" (minimized), the handle comes back
// VK_NULL_HANDLE.
bool CreateSwapchain(const VulkanInstance& inst,
                     const VulkanDevice& dev,
                     VkSurfaceKHR surface,
                     VkSurfaceFormatKHR surfaceFormat,
                     VkExtent2D extent,
                     VkSwapchainKHR oldSwapchain,
                     Swapchain* out) noexcept;

// What the (GPU, surface) pair answers
// ----------------------------------------------------------------------------
//
// Two questions, both instance level, both about this window rather than about any
// swapchain -- which is why the answers live in Window. A physical device, not a
// logical one: taking a whole VulkanDevice and reading only .gpu would have the
// signature claim a device is needed when none is.
//
// They differ in how often they are asked. The format is settled once at startup and
// fed back into every recreate; the extent changes whenever the window does.

// Effect: fills in window->surfaceFormat
bool SelectSurfaceFormat(const VulkanInstance& inst,
                         VkPhysicalDevice gpu,
                         Window* window) noexcept;

// Effect: fills in window->surfaceExtent
// Output: false while minimized, when the surface reports 0x0. A state, not an error
//
// Asked at the top of the loop. Skipping the frame there is what keeps EnsureSwapchain
// from retrying a device wait, a surface query and a creation every iteration while
// minimized, with no present to pace it -- measured at 10.9% CPU against 135.9%.
//
// **The one place a window's size is asked.** It used to be two: glfwGetFramebufferSize
// for the minimize check, and caps.currentExtent inside swapchain creation, which made
// the size a by-product of remaking the swapchain rather than something askable.
bool QuerySurfaceExtent(const VulkanInstance& inst,
                        VkPhysicalDevice gpu,
                        Window* window) noexcept;

// Output: what a pass drawing into a swapchain image is compiled against
//
// **Received, not chosen.** All three fields are the presentation engine's answer: it
// hands back single-sample colour images with no depth, and SelectSurfaceFormat
// settled which colour. That is the opposite of the targets we make, where the same
// type carries decisions -- and the type does not say which, so the call does.
//
// Takes the window and not a Swapchain. The pipelines that bake this are built before
// the first swapchain exists, since EnsureSwapchain runs from BeginFrame.
//
// Contract: SelectSurfaceFormat has run.
AttachmentFormats SwapchainAttachmentFormats(const Window& window) noexcept;

// Effect: remakes window->swapchain when it is out of date or absent
// Output: false means "nowhere to draw right now", and whether that is a failure is
//         **the caller's to decide**. During main's setup it means the program cannot
//         start; in BeginFrame it means minimized, and only that frame is skipped.
//
// No instance parameter -- the window holds the one it was made with. Taking a second
// one would make handing over a different instance expressible, and nothing would
// catch it.
bool EnsureSwapchain(const VulkanDevice& dev, Window* window) noexcept;

// A swapchain that has been replaced and cannot be destroyed yet: present may still
// be reading its images, and **present has no completion signal** -- vkQueuePresentKHR
// hands back no fence.
//
// So there is nothing to wait on and only something to count: once as many presents
// have happened on the new swapchain as it has images, whatever was displaying an old
// one has been replaced. **That is counting, not proof.** The exact answer is the
// present fence in VK_KHR_swapchain_maintenance1, and that the extension had to be
// written is itself the evidence there was no answer before.
//
// Counting wrong is not silent: destroying a swapchain still in use is a VUID
// violation and the validation layer reports it. That is the only check this number
// has.
struct RetiredSwapchain {
    std::unique_ptr<Swapchain> swapchain;
    uint32_t framesLeft = 0;
};

// Effect: destroys the retired swapchains whose count has run out. Once per frame.
//
// The first place here where a resource leaves the destructor ordering: when the owner
// lets go and when the thing is actually destroyed have come apart, and a frame count
// is what bridges them.
void AdvanceRetiredSwapchains(Window* window) noexcept;
