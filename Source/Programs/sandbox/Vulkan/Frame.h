#pragma once

// FrameSlot and FrameTarget - what a frame runs on, and where it goes
// ============================================================================
//
// Two axes, counted differently, which is why 2 slots and 3 images need not match:
//
//   FrameSlot     what one frame's execution needs. slots[] is cycled through and a
//                 frame borrows one; the fence says when one is free again
//   FrameTarget   where that frame ends up. One acquire's worth, made here
//
// Recording sits between BeginFrame and SubmitFrame and is absent from this file --
// it is in Passes, which nothing under Vulkan/ knows about.
//
// What waits on what, in order:
//
//   BeginFrame     wait    inFlight         this frame's last submit is done
//                  acquire                  signals imageAvailable
//   SubmitFrame    reset   inFlight
//                  wait    imageAvailable   at COLOR_ATTACHMENT_OUTPUT
//                  submit                   signals renderFinished and inFlight
//   PresentFrame   wait    renderFinished

#include "Vulkan/Commands.h"
#include "Vulkan/Texture.h"
#include "Vulkan/Window.h"


// cmd, imageAvailable and inFlight are one per frame in flight because one signal
// -- the fence -- says when all three may be reused.
//
// renderFinished is not here: nothing reports when a present finished, so its only
// reuse signal is that image coming back from acquire. Counted by images, it lives in
// Swapchain and reaches a frame through FrameTarget.
struct FrameSlot {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    // From the graphics pool, freed with it. Never returned individually.
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;

    // Which frame of every pass this slot is. Sets and attachments live in the
    // passes, so this number is all the slot needs to find its share of them.
    uint32_t index = 0;


    FrameSlot() = default;
    ~FrameSlot();
    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;
};

// Effect: allocates the command buffer, semaphore and fence for one slot
//
// No pass reaches in here: the sets belong to the passes that fill them, and index is
// all that ties a slot to its share of one.
bool CreateFrameSlot(const VulkanDevice& dev, const Commands& commands,
                     uint32_t index, FrameSlot* out) noexcept;

// Where this frame ends up -- the other axis from FrameSlot.
//
// Counted by swapchain images, not by frames in flight. A slot is what a frame runs
// on; a target is where it goes.
//
// A value assembled by BeginFrame from one acquire, not a pointer into the swapchain's
// array. That is what keeps the three from being paired wrongly, and it is why the
// caller never names a swapchain type: what leaves here is a destination, not an image
// the swapchain owns.
struct FrameTarget {
    const Texture* texture = nullptr;             // draw here
    VkSemaphore renderFinished = VK_NULL_HANDLE;  // signalled once that is done
    uint32_t index = 0;                           // which image present shows
};

// What the caller must do next, not what happened inside. A bool would collapse
// three orders into one and spin forever on the one that never recovers.
enum class FrameResult {
    Ready,   // draw
    Skip,    // no frame this round. Try again next pass (the swapchain is stale)
    Fatal,   // unrecoverable, end the loop (DEVICE_LOST, SURFACE_LOST, OOM)
};

// Input:  dev, window, slot
// Output: target. Emptied first, so Skip and Fatal leave texture null rather than
//         stale -- a caller that ignores the result dereferences null instead of
//         drawing into the frame before's image
// Effect: rebuilds the swapchain if needed, waits for this slot, acquires an image
//
// This is the only place a slot and a target meet: the acquire is made with the
// slot's semaphore, so the pairing is produced here rather than chosen by the caller.
// slot is const because nothing in it changes -- the wait and the acquire only read
// the fence and the semaphore.
//
// Minimization is not handled here: the loop filters it with WindowHasDrawableSize.
FrameResult BeginFrame(const VulkanDevice& dev,
                       Window* window,
                       const FrameSlot& slot,
                       FrameTarget* target) noexcept;

// Submit and present stay two calls: they share only the target and grow on different
// axes -- present per window, submit per queue. The start does not split because
// acquire touches both at once.

// Effect: resets the fence and submits the slot's command buffer
//
// The target rather than a semaphore: what to signal is a property of where this
// frame goes, and passing it whole keeps the caller from having to know that.
bool SubmitFrame(const VulkanDevice& dev, const FrameSlot& slot,
                 const FrameTarget& target) noexcept;

// Effect: presents this frame's target, flagging the window if it went stale
//
// No slot: the index and the semaphore both ride in the target, and present waits on
// the queue rather than on anything this frame owns.
bool PresentFrame(const VulkanDevice& dev,
                  Window* window,
                  const FrameTarget& target) noexcept;
