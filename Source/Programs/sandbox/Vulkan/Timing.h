#pragma once

// GpuTimer - when the GPU reached each point in a frame
// ============================================================================
//
// A CPU clock around a submit measures how long recording took, not how long the GPU
// worked: the commands are still queued when the call returns. A timestamp is written by
// the GPU itself as it reaches a stage, so the difference between two of them is time
// spent on the device.
//
//   what                          where it comes from
//   -----------------------------------------------------------------------
//   the tick values               vkCmdWriteTimestamp2, into a query pool
//   nanoseconds per tick          VkPhysicalDeviceLimits::timestampPeriod
//   how many bits of a tick count queue family's timestampValidBits. 0 means this
//                                 queue cannot do it at all
//
// One pool per frame in flight, for the reason the command buffer and the fence are:
// the results of a submit are not readable until that submit has finished, and the fence
// is what says so. Reading this slot's pool right after its fence is waited on gives the
// numbers from the frame before last, which is what a panel showing them is showing.
//
// Every index is fixed and reset every recording, so a pass that did not run leaves its
// two queries unwritten. Reading asks for availability alongside the value, which is how
// "this pass did not run" is told apart from "this pass took no time".

#include "Vulkan/Commands.h"
#include "Vulkan/Device.h"

struct GpuTimer {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    // VK_NULL_HANDLE when the graphics queue reports no valid timestamp bits. Every
    // function here is then a no-op and reads report nothing, so a device without
    // timestamps loses the measurement rather than the frame.
    VkQueryPool pool = VK_NULL_HANDLE;
    uint32_t capacity = 0;

    // Nanoseconds per tick, and the mask of bits the queue actually fills.
    float period = 0.0f;
    uint64_t validMask = 0;

    GpuTimer() = default;
    ~GpuTimer();
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;
};

// Input:  count is how many timestamps one frame may write
// Output: false only on an allocation failure. A queue with no timestamp support is not
//         a failure -- out is left with a null pool and everything below tolerates it
//
// The pool is reset here, on a one-shot buffer, because a query that has never been
// reset cannot even be read: the first frame asks for results before its own reset has
// executed, and the validation layer refuses that. Reset and never written is the state
// that reads back as unavailable, which is what "this pass has not run yet" means.
bool CreateGpuTimer(const VulkanDevice& dev, const Commands& commands, uint32_t count,
                    GpuTimer* out) noexcept;

// Effect: appends the reset every query needs before it is written again
//
// Contract: called once per recording, before any write, and the pool must not be in use
//           by a submit that has not finished. The caller waited on this slot's fence.
void ResetGpuTimer(const GpuTimer& timer, VkCommandBuffer cmd) noexcept;

// Effect: appends a timestamp that the GPU writes when it reaches stage
//
// The stage decides what the value means. TOP_OF_PIPE is written as soon as the command
// is reached, which on a busy queue is before the work in front of it has finished;
// ALL_COMMANDS is written once everything recorded earlier is done. Two ALL_COMMANDS
// stamps therefore bracket what happened between them, which is what a per-pass cost is.
void WriteGpuTimestamp(const GpuTimer& timer, VkCommandBuffer cmd, uint32_t index,
                       VkPipelineStageFlags2 stage) noexcept;

// Output: false when that query was never written, or the results are not back yet
bool ReadGpuTimestamp(const GpuTimer& timer, uint32_t index, uint64_t* ticks) noexcept;

// Output: milliseconds between two tick values, or 0 when timing is unavailable
double GpuMillis(const GpuTimer& timer, uint64_t begin, uint64_t end) noexcept;
