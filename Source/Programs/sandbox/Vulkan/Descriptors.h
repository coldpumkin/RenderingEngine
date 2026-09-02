#pragma once

// Descriptor - shader가 image를 읽게 하는 장치
// ============================================================================
//
// Image는 push constant처럼 command buffer에 실려 갈 수 없어서, "shader의 N번
// 자리에 이 view를 걸어둔다"를 미리 만들어 두고 bind한다.
//
// 수명이 다른 것들이 모여 있다:
//   sampler     한 번 만들고 끝. 어떤 image든 같은 규칙으로 읽는다
//   setLayout   set 하나의 모양. pipeline layout이 참조하고 set이 이걸로 나온다
//   pool        set이 나오는 곳. 최대 개수를 미리 정하고 자라지 않는다
//   set         여기서 안 산다. Frame과 Texture가 하나씩 들고 있다
//
// 앞의 셋이 프로그램 내내 안 변해서 한 덩어리다. sampler가 여럿이 되면 갈린다.
//
// **setLayout은 shader마다 하나고, 모양도 개수도 그 .spv에서 읽어 만든다.**

#include "Vulkan/Device.h"

// A set layout and the binding count it was built from. The handle is opaque, so the
// count cannot be asked back for.
struct DescriptorLayout {
    VkDescriptorSetLayout handle = VK_NULL_HANDLE;
    uint32_t bindingCount = 0;
};

struct Descriptors {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    VkSampler sampler = VK_NULL_HANDLE;

    DescriptorLayout scene;
    DescriptorLayout present;

    VkDescriptorPool pool = VK_NULL_HANDLE;

    Descriptors() = default;
    ~Descriptors();
    Descriptors(const Descriptors&) = delete;
    Descriptors& operator=(const Descriptors&) = delete;
};

// Input:  각 fragment shader의 .spv 경로(layout 모양이 거기서 나온다)와, 그
//         layout으로 뽑을 set의 개수. **두 개수는 set이 가리킬 image의 개수다** -
//         scene은 texture마다, present는 frame의 resolve마다.
//
//   maxSets          set의 개수         = sceneSets + presentSets
//   descriptorCount  descriptor의 개수  = 가중합. set마다 binding 수가 다르다
//
// Contract: 여기 넘긴 shader가 그 layout으로 만들 pipeline의 shader와 같아야 한다.
bool CreateDescriptors(const VulkanDevice& dev,
                       const char* sceneFragPath, uint32_t sceneSets,
                       const char* presentFragPath, uint32_t presentSets,
                       Descriptors* out) noexcept;

// Output: a set of that layout naming this view (VK_NULL_HANDLE on failure)
// Contract: 개별 반납은 없다. pool이 죽을 때 같이 사라진다.
//           binding이 하나인 layout에만 쓴다 - 아니면 실패한다.
VkDescriptorSet AllocateImageSet(const Descriptors& descriptors,
                                 const DescriptorLayout& layout,
                                 VkImageView view) noexcept;
