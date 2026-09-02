#pragma once

// FrameSlot - one frame, and the pass it runs from end to end
// ============================================================================
//
// A slot, not a frame: slots[] is cycled through, and a frame borrows one. It holds
// everything that pass runs on except the pipelines -- those are the contract it was
// built against, not a resource, and every slot would hold the same two.
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

#include "Vulkan/Attachments.h"
#include "Vulkan/Buffer.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Commands.h"
#include "Vulkan/Texture.h"
#include "Vulkan/Window.h"

struct Mesh;
struct DrawItem;


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

    // Where the frame using this slot draws: our attachments, not swapchain ones.
    // color carries its own resolve and the set that reads it; depth has neither,
    // and that difference is in their descs, not in code here.
    Texture color;
    Texture depth;

    // What the scene brings. Pointers because the scene owns them and every slot
    // reads the same ones -- how many there are, and their memory, is not ours.
    //
    // They leave when there is more than one of either: the array becomes the
    // scene's and a DrawItem picks by index.
    const Mesh* mesh = nullptr;
    const Texture* input = nullptr;

    // scene은 값이고 uniform은 그 GPU 사본이다 - Texture의 desc와 image처럼 짝이다.
    // 프레임마다 CPU가 쓰므로 slot마다 하나다: GPU가 이전 프레임의 것을 읽는 동안
    // 다음 프레임이 자기 것에 쓴다.
    SceneUniform scene{};
    Buffer uniform;

    // set은 pool이 미리 다 뽑아뒀고, 이 slot 몫은 자기 번호로 정해진다. 그래서 set을
    // 들고 다니지도, 넘겨받지도 않는다.
    const Descriptors* descriptors = nullptr;
    uint32_t index = 0;

    // This frame's contents. BeginFrame fills the image, the loop fills the rest, and
    // all of it holds until Present -- recording reads the slot and nothing else.
    const SwapchainImage* image = nullptr;   // 크기는 image->texture.desc.extent다
    const DrawItem* items = nullptr;
    uint32_t itemCount = 0;


    FrameSlot() = default;
    ~FrameSlot();
    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;
};

// Effect: allocates the command buffer, semaphore, fence and the two attachments,
//         and points the slot at what the scene brings.
//
// Contract: formats and extent must be what the pipelines were given. main chooses
//           once and hands the same values to both.
bool CreateFrameSlot(const VulkanDevice& dev, const Commands& commands,
                 const Descriptors& descriptors, uint32_t index,
                 AttachmentFormats formats, VkExtent2D extent,
                 const Mesh& mesh, const Texture& input,
                 FrameSlot* out) noexcept;

// What the caller must do next, not what happened inside. A bool would collapse
// three orders into one and spin forever on the one that never recovers.
enum class FrameResult {
    Ready,   // draw
    Skip,    // no frame this round. Try again next pass (the swapchain is stale)
    Fatal,   // unrecoverable, end the loop (DEVICE_LOST, SURFACE_LOST, OOM)
};

// Input:  dev, window, slot
// Effect: rebuilds the swapchain if needed, waits for this slot, acquires an image
//         into it. slot->image is valid only on Ready.
//
// Minimization is not handled here: the loop filters it with WindowHasDrawableSize.
FrameResult BeginFrame(const VulkanDevice& dev,
                       Window* window,
                       FrameSlot* slot) noexcept;

// Submit and present stay two calls: they share no arguments and grow on different
// axes -- present per window, submit per queue. The start does not split because
// acquire touches both at once.

// Effect: resets the fence and submits the slot's command buffer
//
// Takes the pair so the slot and the image it signals cannot be mismatched.
bool SubmitFrame(const VulkanDevice& dev, const FrameSlot& slot) noexcept;

// Effect: presents the swapchain image, flagging the window if it went stale
//
// The index rides inside slot.image, so there is nothing else to pass.
bool PresentFrame(const VulkanDevice& dev,
                  Window* window,
                  const FrameSlot& slot) noexcept;
