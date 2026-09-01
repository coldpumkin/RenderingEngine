#pragma once

// Frame - one set of what a frame needs, plus opening and closing it
// ============================================================================
//
// Separate from Commands because the counts come from different places: a pool
// is per queue family, a Frame is per frame in flight.
//
// What waits on what, in order:
//
//   BeginFrame     wait    inFlight         this frame's last submit is done
//                  acquire                  signals imageAvailable
//   SubmitFrame    reset   inFlight
//                  wait    imageAvailable   at COLOR_ATTACHMENT_OUTPUT
//                  submit                   signals renderFinished and inFlight
//   PresentFrame   wait    renderFinished
//
// Recording happens between BeginFrame and SubmitFrame and appears nowhere in
// this file: drawing and synchronization change for different reasons.

#include "Vulkan/Commands.h"
#include "Vulkan/RenderTargets.h"
#include "Vulkan/Window.h"

// Three things sized by kFramesInFlight, because one signal answers for all of
// them. The question is "what tells us this may be reused?":
//
//   cmd             cannot be reset while pending  -> the inFlight fence
//   imageAvailable  cannot re-signal until its wait is done -> the inFlight fence
//   inFlight        is that signal
//
// renderFinished is absent for the same reason. Present waits on it, and nothing
// reports when present is done - vkQueuePresentKHR hands back no fence. The only
// hint is acquire returning that image again, which arrives as an image index,
// so it is sized by image count and lives in Swapchain. That is why 2 frames and
// 3 images need not match: they are never paired.
struct Frame {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    // From the graphics pool, freed with it. Never returned individually.
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;

    // Where this frame draws: our image, not a swapchain one.
    RenderTargets targets;

    // How the present pass reads targets.resolve. A scene resource and its present
    // set are different passes' business, so the set sits beside the targets rather
    // than inside them. main fills it; the pool frees it.
    VkDescriptorSet resolveSet = VK_NULL_HANDLE;

    Frame() = default;
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

// Effect: allocates the command buffer, semaphore, fence and render targets. The
//         resolveSet is not filled here - main allocates it from Descriptors.
//
// Contract: formats and extent must be what the pipelines were given. main chooses
//           once and hands the same values to both.
bool CreateFrame(const VulkanDevice& dev, const Commands& commands,
                 RenderTargetFormats formats, VkExtent2D extent,
                 Frame* out) noexcept;

// Where this frame draws and where it presents. BeginFrame fills it in.
//
// The two being separate is what off-screen rendering amounts to:
//   draw     our image. Works with no swapchain at all
//   present  the swapchain image the result is copied to
//
// It is a struct because recording sits between acquire and present, so a local
// cannot carry the choice across. The index present needs rides inside
// SwapchainImage rather than beside it.
struct FrameTarget {
    const RenderTargets* draw = nullptr;
    VkDescriptorSet drawResolveSet = VK_NULL_HANDLE;   // present reads draw->resolve
    const SwapchainImage* present = nullptr;
    VkExtent2D presentExtent{};   // window size, which may differ from draw->extent
};

// Written as what the caller must do next, not as what happened inside.
//
// A bool cannot carry this. false would stand for three different orders - sleep,
// retry now, stop - and a caller that cannot tell them apart retries all three,
// which spins forever on the one that never recovers.
enum class FrameResult {
    Ready,   // draw
    Skip,    // no frame this round. Try again next pass (the swapchain is stale)
    Fatal,   // unrecoverable, end the loop (DEVICE_LOST, SURFACE_LOST, OOM)
};

// Input:  dev, window, frame
// Output: target (valid only on Ready)
// Effect: rebuilds the swapchain if needed, waits for this frame, acquires an image
//
// Minimization is not handled here: the loop filters it with WindowHasDrawableSize.
FrameResult BeginFrame(const VulkanDevice& dev,
                       Window* window,
                       const Frame& frame,
                       FrameTarget* out) noexcept;

// The end of a frame - where the window and the frame part ways
// ---------------------------------------------------------------------------
// Two calls rather than one:
//
//   the spec already split them   vkQueueSubmit2     core 1.3
//                                 vkQueuePresentKHR  VK_KHR_swapchain
//   they share almost nothing     VkPresentInfoKHR takes a semaphore, a swapchain
//                                 and an image index. No command buffer, no fence
//   they grow on different axes   present per window, submit per queue
//
// The start does not split because acquire touches both at once: the window's
// image index and the frame's own semaphore.

// Input:  dev, frame, signalWhenDone
// Effect: resets the fence and submits frame.cmd to the graphics queue
//
// Knows nothing about the swapchain. Two semaphores are the whole connection to
// the window - it waits on frame.imageAvailable and signals signalWhenDone.
bool SubmitFrame(const VulkanDevice& dev,
                 const Frame& frame,
                 VkSemaphore signalWhenDone) noexcept;

// Input:  dev, window, image
// Effect: presents the swapchain image, flagging the window if it went stale
//
// Takes the image BeginFrame chose rather than looking it up by index again. The
// index present requires comes with the image, so the pair cannot disagree.
bool PresentFrame(const VulkanDevice& dev,
                  Window* window,
                  const SwapchainImage& image) noexcept;
