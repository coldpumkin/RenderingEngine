#pragma once

// ScenePass and FrameSlot - what a pass owns, and what a frame borrows
// ============================================================================
//
// Two axes, and they are not the same question:
//
//   ScenePass   what one pass draws into and reads. Its attachments are per frame
//               in flight because every frame rewrites them
//   FrameSlot   what one frame's execution needs, whichever pass it runs. slots[]
//               is cycled through and a frame borrows one
//
// A slot, not a frame: frames keep coming, slots are reused, and the fence says when
// one is free again. Pipelines are in neither -- they are the contract both were
// built against, and every slot would hold the same two.
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

#include "Config.h"   // kFramesInFlight sizes ScenePass::frames
#include "Vulkan/Attachments.h"
#include "Vulkan/Buffer.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Commands.h"
#include "Vulkan/Texture.h"
#include "Vulkan/Window.h"

struct Mesh;
struct DrawItem;


// ScenePass - the off-screen pass, and what it draws into
// ============================================================================
//
// The pass is one; its attachments are one set per frame in flight. Every frame
// draws into them again, so a frame cannot share them with one the GPU has not
// finished -- the first barrier in recording is srcStage TOP_OF_PIPE, which waits
// for nothing.
//
// What the pass reads sits beside frames[], not inside it: nothing writes a mesh or
// a sampled texture after creation, so every frame reads the same one. Pointers
// because the scene owns them -- how many there are is not this pass's business.
//
// They become arrays the day one pass needs two of either, and a DrawItem then picks
// by index. The bind moves into the draw loop with them.
struct ScenePass {
    const Mesh* mesh = nullptr;
    const Texture* input = nullptr;

    // Non-owning, and a pointer rather than a value: a pass is a render-target
    // configuration with draws in it, and how many pipelines those draws use is not
    // fixed at one. This becomes a list the day a draw here needs a different one.
    //
    // It is here because the pass owns both sides of a pair -- the pipeline bakes in
    // the attachment formats, and frames[].color is made from the same ones.
    const Pipeline* pipeline = nullptr;

    struct PerFrame {
        Texture color;         // multisample. Drawn into, then discarded
        Texture colorResolve;  // 1 sample. vkCmdEndRendering averages into it, and
                               // the post pass samples it -- the one that leaves
        Texture depth;         // multisample. Tested and written, never read outside

        // The value and its GPU copy, paired the way Texture pairs desc and image.
        // Per frame for the other reason the attachments are: the CPU writes this one
        // while the GPU still reads the previous frame's.
        //
        // Every draw in the pass reads the same values -- what differs per draw rides
        // the command buffer as a push constant instead.
        SceneUniform uniformValue{};
        Buffer uniform;

        // Drawn from the pool by this pass and filled by it: the set names this
        // frame's input and uniform, so no one else knows what belongs in it.
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    PerFrame frames[kFramesInFlight];
};

// Effect: creates each frame's attachments and uniform buffer, and points the pass at
//         what the scene brings.
//
// Contract: formats must be what pipeline was built with. Both are arguments here so
//           the mismatch is at least in one call, but nothing checks it.
bool CreateScenePass(const VulkanDevice& dev, const Descriptors& descriptors,
                     AttachmentFormats formats, VkExtent2D extent,
                     const Mesh& mesh, const Texture& input,
                     const Pipeline& pipeline, ScenePass* out) noexcept;


// PostProcessPass - reads what the scene pass produced, writes the swapchain image
// ============================================================================
//
// source is a dependency, not an order. Holding the pointer does not stop anyone from
// recording this pass first; the order is the two lines in RecordFrame and stays
// there. Passes ordered by the CPU is the point -- there is no graph to walk.
//
// No attachments of its own: what it draws into arrives from acquire, sized by the
// swapchain's image count rather than by frames in flight.
//
// The pipeline is const, like the scene pass's. It was mutable while the surface
// format could change under it; the format is settled once at init now, so nothing
// rebuilds this and nothing has to watch for it.
struct PostProcessPass {
    const ScenePass* source = nullptr;
    const Pipeline* pipeline = nullptr;   // non-owning

    // One per frame in flight, because each names that frame's colorResolve. Flat
    // rather than a PerFrame like the scene pass, since a set is all there is.
    VkDescriptorSet sets[kFramesInFlight]{};
};

// Effect: draws this pass's sets and points each at the matching frame of source
//
// Contract: source must already be created -- the sets name its colorResolve images.
bool CreatePostProcessPass(const Descriptors& descriptors, const ScenePass& source,
                           const Pipeline& pipeline, PostProcessPass* out) noexcept;


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

    // Which frame of every pass this slot is. Sets and attachments both live in the
    // passes now, so this number is all the slot needs to find its share of them.
    uint32_t index = 0;


    FrameSlot() = default;
    ~FrameSlot();
    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;
};

// Effect: allocates the command buffer, semaphore and fence for one slot
//
// No pass reaches in here any more: the sets moved to the passes that fill them, and
// index is all that ties a slot to its share of one.
bool CreateFrameSlot(const VulkanDevice& dev, const Commands& commands,
                     uint32_t index, FrameSlot* out) noexcept;

// Where this frame ends up -- the other axis from FrameSlot.
//
// Counted by swapchain images, not by frames in flight, which is why 2 slots and 3
// images need not match. A slot is what a frame runs on; a target is where it goes.
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
