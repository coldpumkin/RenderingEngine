#pragma once

// Descriptor - shader가 image를 읽게 하는 장치
// ============================================================================
//
// Push constant는 값이 command buffer에 실려 간다. Image는 그럴 수 없어서
// "shader의 N번 자리에 이 view를 걸어둔다"를 미리 만들어 두고 bind한다.
//
// 수명이 다른 것들이 모여 있다:
//   sampler     한 번 만들고 끝. 어떤 image든 같은 규칙으로 읽는다
//   setLayout   set 하나의 모양. pipeline layout이 참조하고 set이 이걸로 나온다
//   pool        set이 나오는 곳. 최대 개수를 미리 정하고 자라지 않는다
//   set         여기서 안 산다. Frame과 Texture가 하나씩 들고 있다
//
// 앞의 셋은 프로그램이 사는 동안 안 변해서 여기 한 덩어리다.
// 갈릴 때: sampler가 여럿이 되면 (mipmap·anisotropy·address mode가 다른 texture들).
//
// **setLayout은 pass마다 하나씩이다.** 근거는 shader가 요구하는 모양뿐이다:
//
//   scene    triangle.frag가 sampler2D 하나 (tex)         -> binding 0
//   present  fullscreen.frag가 sampler2D 하나 (sceneColor) -> binding 0
//
// 지금 둘의 모양이 같다.

#include "Vulkan/Device.h"

struct Descriptors {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    VkSampler sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout sceneLayout = VK_NULL_HANDLE;     // binding 0
    VkDescriptorSetLayout presentLayout = VK_NULL_HANDLE;   // binding 0

    VkDescriptorPool pool = VK_NULL_HANDLE;

    Descriptors() = default;
    ~Descriptors();
    Descriptors(const Descriptors&) = delete;
    Descriptors& operator=(const Descriptors&) = delete;
};

// Input:  sceneSets, presentSets - 그 layout으로 뽑을 set의 개수
//
// 두 개수를 따로 받는 이유: 근거가 다르고(texture 개수 / frames-in-flight), pool이
// 재는 것이 둘이라 합만으로는 부족하다.
//
//   maxSets          set의 개수         = sceneSets + presentSets
//   descriptorCount  descriptor의 개수  = set마다 binding 수가 달라 가중합이다
//
// 합쳐서 하나로 받으면 여기서 그 비율을 알 수 없다.
bool CreateDescriptors(const VulkanDevice& dev,
                       uint32_t sceneSets, uint32_t presentSets,
                       Descriptors* out) noexcept;

// Input:  descriptors, view (binding 0)
// Output: sceneLayout 모양의 set (실패하면 VK_NULL_HANDLE)
// Contract: 개별 반납은 없다. pool이 죽을 때 같이 사라진다.
//
// view 개수는 layout이 요구하는 binding 개수다. 안 채운 자리를 shader가 읽으면
// draw에서 잡히므로 인자로 강제한다.
VkDescriptorSet AllocateSceneSet(const Descriptors& descriptors,
                                 VkImageView view) noexcept;

// Input:  descriptors, view (binding 0)
// Output: presentLayout 모양의 set (실패하면 VK_NULL_HANDLE)
// Contract: 개별 반납은 없다. pool이 죽을 때 같이 사라진다.
VkDescriptorSet AllocatePresentSet(const Descriptors& descriptors,
                                   VkImageView view) noexcept;
