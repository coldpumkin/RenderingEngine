#pragma once

// Descriptors - how a shader reaches an image or a buffer
// ============================================================================
//
// An image cannot ride in the command buffer the way a push constant does. So the
// binding is built ahead of time -- "slot N of this set names this view" -- and the
// command buffer carries only which set to use.
//
// Two things, kept together because neither changes for the life of the program:
//   sampler   made once. Every image is read by the same rule
//   pool      where sets come from. Its size is fixed up front and never grows
//
// No sets and no pass names here. A pool has to be sized before anything can be
// drawn from it, so every pass says up front how many it wants; after that each pass
// draws its own and holds them. Adding a pass then changes that pass and this file's
// caller, not this file.
//
// The set layouts are not here either: a shader declares them and the pipeline built
// from that shader owns them. This only borrows them to size the pool.

#include "Vulkan/Buffer.h"
#include "Vulkan/Image.h"
#include "Vulkan/Shader.h"

// What to put in one binding. The layout's type decides which field is read, so a
// caller filling the wrong one is caught by the validation layer, not here.
//
// The resources themselves and not their handles: a buffer knows how big it is, so
// the range a descriptor covers is read off it rather than written beside it. Five
// call sites used to pass sizeof(TheBlock) next to the handle, which is one value in
// two places and nothing compared them.
struct BindingValue {
    const ImageView* view = nullptr;   // image types
    const Buffer* buffer = nullptr;    // buffer types
};

struct Descriptors {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    // What the pool was built for. Kept because **Vulkan has no way to ask**: the
    // sizes go into vkCreateDescriptorPool and are never readable again, so a panel
    // that wants to show them has no other source.
    //
    // Two different things, which is the whole reason the pool takes both: how many
    // sets can be drawn, and how many descriptors those sets contain. A layout with
    // two bindings spends one set and two descriptors.
    uint32_t maxSets = 0;
    uint32_t imageDescriptors = 0;    // COMBINED_IMAGE_SAMPLER
    uint32_t bufferDescriptors = 0;   // UNIFORM_BUFFER

    Descriptors() = default;
    ~Descriptors();
    Descriptors(const Descriptors&) = delete;
    Descriptors& operator=(const Descriptors&) = delete;
};

// One claim on the pool, made before any set exists.
//
// count is whatever that layout is counted by, and our two differ: a frame's set is
// one per frame in flight, a material's is one per material. That difference is the
// reason they are two layouts and not two bindings in one.
struct SetRequest {
    const DescriptorLayout* layout = nullptr;
    uint32_t count = 0;
};

// Input:  every request, so the pool can be sized once and never grow
//
//   maxSets          how many sets        = the counts summed
//   descriptorCount  how many descriptors = weighted: layouts differ in binding count
//
// Contract: the layouts must outlive this, and a set drawn from one is only ever
//           bound with the pipeline that owns that layout.
bool CreateDescriptors(const VulkanDevice& dev,
                       const SetRequest* requests, uint32_t requestCount,
                       Descriptors* out) noexcept;

// Effect: draws count sets of one layout out of the pool
//
// Contract: the total across all calls must match what CreateDescriptors was told --
//           the pool does not grow, so overshooting fails here rather than earlier.
bool AllocateSets(const Descriptors& descriptors, const DescriptorLayout& layout,
                  uint32_t count, VkDescriptorSet* out) noexcept;

// Effect: fills in what an already-allocated set points at
//
// Contract: count must equal layout.bindingCount, and the GPU must not be reading the
//           set -- every call here happens at startup, so that moment never arrives.
void UpdateSet(const Descriptors& descriptors, const DescriptorLayout& layout,
               VkDescriptorSet set,
               const BindingValue* values, uint32_t count) noexcept;
