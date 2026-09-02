#pragma once

// FrameSlot - one frame, and the pass it runs from end to end
// ============================================================================
//
// A slot, not a frame: slots[] is cycled through, and a frame borrows one. It holds
// what that pass runs on, so recording takes no resources as arguments.
//
// Two pipelines in a row are one pass here: the scene draws off-screen, then present
// samples that into the swapchain. A post effect goes in that second stage.
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
// Recording sits between BeginFrame and SubmitFrame and is absent here.

#include "Vulkan/Commands.h"
#include "Vulkan/Attachments.h"
#include "Vulkan/Texture.h"
#include "Vulkan/Window.h"

struct Pipeline;

// cmd, imageAvailable and inFlight are sized by kFramesInFlight because one signal
// -- the fence -- says when all three may be reused.
//
// renderFinished is not here: nothing reports when a present finished, so its only
// reuse signal is that image coming back from acquire. Sized by image count, it
// lives in Swapchain. That is why 2 slots and 3 images need not match.
struct FrameSlot {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    // From the graphics pool, freed with it. Never returned individually.
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;

    // Where the frame using this slot draws: our images, not swapchain ones. A set
    // means the next stage reads it, so resolve has one and the other two do not.
    VkExtent2D extent{};        // render resolution, not the window's
    Texture color;              // MSAA. Averaged into resolve and dropped
    Texture resolve;            // 1-sample. The present stage samples it
    Texture depth;              // MSAA. Never leaves the frame

    // The two stages in order. What the second one samples is resolve, which carries
    // its own set; what the first one samples comes from the scene.
    const Pipeline* scene = nullptr;
    const Pipeline* present = nullptr;

    FrameSlot() = default;
    ~FrameSlot();
    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;
};

// Effect: allocates the command buffer, semaphore, fence and the three images. The
//         two stages are not filled here - main does that.
//
// Contract: formats and extent must be what the pipelines were given. main chooses
//           once and hands the same values to both.
bool CreateFrameSlot(const VulkanDevice& dev, const Commands& commands,
                 AttachmentFormats formats, VkExtent2D extent,
                 FrameSlot* out) noexcept;

// The slot a frame borrowed and the image acquire gave it. Recording sits between
// acquire and present, so the pair has to be carried, not held in a local.
struct AcquiredFrame {
    const FrameSlot* slot = nullptr;
    const SwapchainImage* image = nullptr;
    VkExtent2D extent{};          // window size, unlike slot->extent
};

// What the caller must do next, not what happened inside. A bool would collapse
// three orders into one and spin forever on the one that never recovers.
enum class FrameResult {
    Ready,   // draw
    Skip,    // no frame this round. Try again next pass (the swapchain is stale)
    Fatal,   // unrecoverable, end the loop (DEVICE_LOST, SURFACE_LOST, OOM)
};

// Input:  dev, window, slot
// Output: out (valid only on Ready)
// Effect: rebuilds the swapchain if needed, waits for this slot, acquires an image
//
// Minimization is not handled here: the loop filters it with WindowHasDrawableSize.
FrameResult BeginFrame(const VulkanDevice& dev,
                       Window* window,
                       const FrameSlot& slot,
                       AcquiredFrame* out) noexcept;

// Submit and present stay two calls: they share no arguments and grow on different
// axes -- present per window, submit per queue. The start does not split because
// acquire touches both at once.

// Effect: resets the fence and submits the slot's command buffer
//
// Takes the pair so the slot and the image it signals cannot be mismatched.
bool SubmitFrame(const VulkanDevice& dev, const AcquiredFrame& acquired) noexcept;

// Effect: presents the swapchain image, flagging the window if it went stale
//
// Takes the image itself, not an index: the index rides inside it.
bool PresentFrame(const VulkanDevice& dev,
                  Window* window,
                  const SwapchainImage& image) noexcept;
