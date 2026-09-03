#pragma once

#include "Vulkan/Device.h"

// Command pools - one per queue family
// ============================================================================
//
// The spec ties the two together: a pool is created for a queueFamilyIndex, and a
// buffer drawn from it may only be submitted to a queue of that same family.
//
// Pool and buffer differ in lifetime. A pool lasts as long as the device; a buffer is
// reset and rewritten every frame, once the GPU is done with the last recording.
//
// How many pools there are is a product of three axes -- queue family (3) x thread (1)
// x frames in flight (1):
//   thread             a pool is not thread-safe, so one per thread that records
//   frames in flight   only if the pool is reset as a whole. We reset buffers
//                      individually (RESET_COMMAND_BUFFER), so this axis is 1
//
// The compute and transfer pools are created and nothing is submitted to them yet.
// Without a dedicated family they are VK_NULL_HANDLE and graphics does that work.
struct Commands {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkCommandPool graphics = VK_NULL_HANDLE;
    VkCommandPool compute  = VK_NULL_HANDLE;
    VkCommandPool transfer = VK_NULL_HANDLE;

    Commands() = default;
    ~Commands();
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;
};

bool CreateCommands(const VulkanDevice& dev, Commands* out) noexcept;

// One-shot command buffers, for initialization only
// ============================================================================
//
// Begin allocates a buffer and opens recording; End closes it, submits, waits for the
// GPU and frees it. The vkCmd* calls go in between.
//
// **Not for a per-frame path.** There is a vkQueueWaitIdle inside -- acceptable while
// nothing else is running, not cheap.
//
// On the graphics queue. What a transfer queue buys is uploading while something else
// draws, and these all happen before the loop, so there is nothing to overlap with --
// only the ownership transfer left to pay for. Tried and reverted (b104a27).

// Output: a command buffer with recording open, or VK_NULL_HANDLE
VkCommandBuffer BeginOneShot(const VulkanDevice& dev, const Commands& commands) noexcept;

// Input:  what - the name a failure line prints
// Effect: frees the command buffer either way
bool EndOneShotAndWait(const VulkanDevice& dev, const Commands& commands,
                       VkCommandBuffer cmd, const char* what) noexcept;
