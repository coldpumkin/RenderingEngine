#pragma once

#include "Vulkan/Instance.h"

#include <vma/vk_mem_alloc.h>

// Picking a physical device, and the queue families on it
// ============================================================================
//
// A GPU groups its queues into families and says what each group can do -- "this one
// does graphics, compute and transfer; that one does transfer alone". A
// transfer-only family is usually a separate DMA engine, running physically parallel
// to graphics.
//
// The spec makes transfer implicit wherever GRAPHICS or COMPUTE is set. So the
// TRANSFER bit does not answer "can this family transfer"; it answers "is this family
// dedicated to transfer", which is the question worth asking.
//
// A desktop GPU typically reports:
//   family 0 : GRAPHICS | COMPUTE | TRANSFER   general purpose
//   family 1 : COMPUTE  | TRANSFER             async compute
//   family 2 : TRANSFER                        DMA engine
struct QueueFamilies {
    uint32_t graphics = UINT32_MAX;   // required, and where present happens too
    uint32_t compute  = UINT32_MAX;   // may be absent
    uint32_t transfer = UINT32_MAX;   // may be absent

    bool HasCompute()  const noexcept { return compute  != UINT32_MAX; }
    bool HasTransfer() const noexcept { return transfer != UINT32_MAX; }
};

// A compute or transfer queue is only taken when its family is dedicated.
//
// The point of a separate one is running alongside graphics. Falling back to the same
// family gives none of that and still pays what splitting queues costs -- semaphores,
// and queue family ownership transfers. Unreal leaves them null in the same case.
//
// Failing to find graphics is a failure; the other two are not.
struct PhysicalDeviceSelection {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    QueueFamilies families;
};

// Prefers a discrete GPU among the ones that qualify.
// On failure the gpu comes back VK_NULL_HANDLE.
PhysicalDeviceSelection PickPhysicalDevice(const VulkanInstance& inst,
                                           VkSurfaceKHR surface) noexcept;

// Logical device + function table + queues
// ============================================================================

// The queue handles. compute and transfer may be VK_NULL_HANDLE, which means no
// dedicated family was found and graphics does that work.
struct Queues {
    VkQueue graphics = VK_NULL_HANDLE;
    VkQueue compute  = VK_NULL_HANDLE;
    VkQueue transfer = VK_NULL_HANDLE;

    // Present is a role, not a fourth queue. It aliases one of the three above and
    // defaults to graphics, the way Unreal's FVulkanQueue* PresentQueue does.
    //
    // Hardware where graphics cannot present is not supported. Unreal puts up a
    // message box and exits in that case.
    //
    // Worth revisiting: on AMD there is a faster path presenting from the compute
    // queue. It changes the submit structure, so it waits until latency is something
    // we can actually measure.
    VkQueue present = VK_NULL_HANDLE;

    VkQueue ComputeOrGraphics()  const noexcept { return compute  ? compute  : graphics; }
    VkQueue TransferOrGraphics() const noexcept { return transfer ? transfer : graphics; }
};

// The device level, five things in one.
//
// Once the device exists, gpu, families and queues are never used without it -- and
// almost everything that uses the device uses the table in the same breath.
struct VulkanDevice {
    VolkDeviceTable table{};
    VkDevice handle = VK_NULL_HANDLE;

    // The instance this was made from, non-owning -- the same way every type below
    // holds its device. It is what lets device-level code ask an instance-level
    // question about the GPU it is running on, which image creation needs: whether a
    // format can do what a usage declares.
    const VulkanInstance* inst = nullptr;

    // What the selection found, absorbed here. Nothing to destroy, so nothing owned.
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    QueueFamilies families;

    // vkGetDeviceQueue is a query. vkCreateDevice already made these, and there is no
    // function that destroys one.
    Queues queues;

    // This GPU's memory types. Kept rather than asked for again because there is one
    // answer and it never changes, not because the query is out of reach -- inst above
    // is what makes it reachable.
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    // No attachment format here, and none in the selection either: the candidate list
    // and its order are our render target's policy rather than a property of the GPU.
    // Attachments.h decides, and whoever draws calls it. This file does not include
    // that header at all.

    // The GPU memory allocator, made by the device and destroyed with it.
    VmaAllocator allocator = VK_NULL_HANDLE;

    VulkanDevice() = default;
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
};

// On failure the handle comes back VK_NULL_HANDLE.
//
// It takes an instance because vkCreateDevice is an instance-level function while
// vkDestroyDevice is device-level. The API is asymmetric there, which is the reason
// the question to ask is what level a *call* is, not what level a type is.
bool CreateDevice(const VulkanInstance& inst,
                  const PhysicalDeviceSelection& selection,
                  VulkanDevice* out) noexcept;
