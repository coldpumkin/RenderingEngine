#pragma once

// Descriptor - shader가 image를 읽게 하는 장치
// ============================================================================
//
// Push constant는 값이 command buffer에 실려 간다. Image는 그럴 수 없어서
// "shader의 0번 자리에 이 view를 걸어둔다"를 미리 만들어 두고 bind한다.
//
// 넷의 수명이 다르다:
//   sampler     한 번 만들고 끝. 어떤 image든 같은 규칙으로 읽는다
//   setLayout   "0번은 image+sampler 하나"라는 모양. pipeline layout이 참조한다
//   pool        set을 담는 그릇. 최대 개수를 미리 정한다
//   set         frames-in-flight마다 하나 - Frame이 들고 있다
//
// 앞의 셋은 프로그램이 사는 동안 안 변해서 여기 한 덩어리다.
// 갈릴 때: sampler가 여럿이 되거나(mipmap·anisotropy·address mode가 다른 texture들)
// setLayout이 여럿이 될 때.

#include "Vulkan/Device.h"

struct Descriptors {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    Descriptors() = default;
    ~Descriptors();
    Descriptors(const Descriptors&) = delete;
    Descriptors& operator=(const Descriptors&) = delete;
};

// maxSets: pool에서 뽑을 수 있는 최대 개수. pool은 자라지 않아서 미리 정해야 한다.
bool CreateDescriptors(const VulkanDevice& dev, uint32_t maxSets, Descriptors* out) noexcept;

// Input:  descriptors, view
// Output: view가 걸린 set (실패하면 VK_NULL_HANDLE)
// Contract: 개별 반납은 없다. pool이 죽을 때 같이 사라진다.
VkDescriptorSet AllocateImageSet(const Descriptors& descriptors, VkImageView view) noexcept;
