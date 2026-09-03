#pragma once

// Descriptor - shader가 image를 읽게 하는 장치
// ============================================================================
//
// Image는 push constant처럼 command buffer에 실려 갈 수 없어서, "shader의 N번
// 자리에 이 view를 걸어둔다"를 미리 만들어 두고 bind한다.
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

#include "Vulkan/Device.h"
#include "Vulkan/Shader.h"

// What to put in one binding. The layout's type decides which field is read, so a
// caller filling the wrong one is caught by the validation layer, not here.
struct BindingValue {
    VkImageView view = VK_NULL_HANDLE;   // image types
    VkBuffer buffer = VK_NULL_HANDLE;    // buffer types
    VkDeviceSize size = 0;
};

struct Descriptors {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

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

// Effect: 이미 뽑아둔 set이 무엇을 가리키는지 채운다
//
// Contract: count가 layout.bindingCount와 같아야 한다. GPU가 그 set을 읽는 중이면
//           안 된다 - 지금은 초기화 때 한 번뿐이라 그 순간이 없다.
void UpdateSet(const Descriptors& descriptors, const DescriptorLayout& layout,
               VkDescriptorSet set,
               const BindingValue* values, uint32_t count) noexcept;
