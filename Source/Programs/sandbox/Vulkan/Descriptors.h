#pragma once

// Descriptor - shader가 image를 읽게 하는 장치
// ============================================================================
//
// Image는 push constant처럼 command buffer에 실려 갈 수 없어서, "shader의 N번
// 자리에 이 view를 걸어둔다"를 미리 만들어 두고 bind한다.
//
// Things with different lifetimes, kept together because none of them changes for
// the life of the program:
//   sampler   made once. Every image is read by the same rule
//   pool      where sets come from. Its size is fixed up front and never grows
//   set       all drawn when the pool is made, one per slot
//
// The set layouts are not here: a shader declares them and the pipeline built from
// that shader owns them. This only borrows them to size the pool and draw the sets.

#include "Vulkan/Device.h"
#include "Vulkan/Shader.h"

// What to put in one binding. The layout's type decides which field is read, so a
// caller filling the wrong one is caught by the validation layer, not here.
struct BindingValue {
    VkImageView view = VK_NULL_HANDLE;   // image types
    VkBuffer buffer = VK_NULL_HANDLE;    // buffer types
    VkDeviceSize size = 0;
};

// Set은 pool을 만들 때 다 뽑는다 - 개수가 그때 이미 정해져 있다. 밖으로 나가는 것은
// 핸들이 아니라 slot의 번호이고, 그래서 어느 자원도 set을 들지 않는다.
constexpr uint32_t kMaxSetsPerLayout = 4;

struct Descriptors {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    VkSampler sampler = VK_NULL_HANDLE;

    // Non-owning: the pipelines made them, and they outlive this.
    const DescriptorLayout* scene = nullptr;
    const DescriptorLayout* present = nullptr;

    VkDescriptorPool pool = VK_NULL_HANDLE;

    // 할당만 된 상태로 온다. 무엇을 가리키는지는 UpdateSet이 채운다 - 그 자원들이
    // 아직 없을 때 pool이 만들어지기 때문이다.
    VkDescriptorSet sceneSets[kMaxSetsPerLayout]{};
    VkDescriptorSet presentSets[kMaxSetsPerLayout]{};

    Descriptors() = default;
    ~Descriptors();
    Descriptors(const Descriptors&) = delete;
    Descriptors& operator=(const Descriptors&) = delete;
};

// Input:  the layout each pipeline built, and how many sets to draw from it. Both
//         counts are per frame in flight.
//
//   maxSets          how many sets        = sceneSets + presentSets
//   descriptorCount  how many descriptors = weighted: layouts differ in binding count
//
// Contract: both layouts must outlive this, and the sets are only ever bound with
//           the pipeline that owns theirs.
bool CreateDescriptors(const VulkanDevice& dev,
                       const DescriptorLayout& scene, uint32_t sceneSets,
                       const DescriptorLayout& present, uint32_t presentSets,
                       Descriptors* out) noexcept;

// Effect: 이미 뽑아둔 set이 무엇을 가리키는지 채운다
//
// Contract: count가 layout.bindingCount와 같아야 한다. GPU가 그 set을 읽는 중이면
//           안 된다 - 지금은 초기화 때 한 번뿐이라 그 순간이 없다.
void UpdateSet(const Descriptors& descriptors, const DescriptorLayout& layout,
               VkDescriptorSet set,
               const BindingValue* values, uint32_t count) noexcept;
