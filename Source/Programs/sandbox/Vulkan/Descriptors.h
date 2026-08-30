#pragma once

// ============================================================================
// 셰이더가 이미지를 읽게 하는 장치
// ============================================================================
//
// 푸시 상수는 값이 커맨드 버퍼에 실려 갔다. 이미지는 그럴 수 없어서 **"이 셰이더의
// 0번 자리에 이 뷰를 걸어둔다"를 미리 만들어 놓고 바인딩**한다. 그게 디스크립터다.
//
// ---------------------------------------------------------------------------
// **수명이 넷 다 다르다** - 우리 기준 셋 중 제일 안 써본 것이 여기서 나온다:
//
//   sampler     한 번 만들고 끝. 어떤 이미지든 같은 규칙으로 읽는다
//   setLayout   "0번은 이미지+샘플러 하나" 라는 모양. 파이프라인 레이아웃이 참조한다
//   pool        셋을 담는 그릇. 최대 개수를 미리 정해서 만든다
//   set         **frames-in-flight마다 하나** - 각 프레임이 자기 오프스크린을 읽으니까
//
// 앞의 셋은 프로그램이 사는 동안 안 변해서 여기 한 덩어리로 있고, **set만 Frame이
// 들고 있다.** 개수의 근거가 다르기 때문이다.
//
// **덩어리로 둔 근거가 약해지는 때**: 샘플러가 여럿이 되거나(밉맵·비등방·클램프 모드가
// 달라지는 텍스처들), 셋 레이아웃이 여럿이 될 때. 그때 셋으로 갈린다.
// ---------------------------------------------------------------------------

#include "Vulkan/Device.h"

struct Descriptors {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 비소유 상태

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    Descriptors() = default;
    ~Descriptors();
    Descriptors(const Descriptors&) = delete;
    Descriptors& operator=(const Descriptors&) = delete;
};

// maxSets: 풀에서 몇 개까지 뽑을 수 있나. **미리 정해야 한다** - 풀은 자라지 않는다.
bool CreateDescriptors(const VulkanDevice& dev, uint32_t maxSets, Descriptors* out) noexcept;

// 셋 하나를 뽑아서 view를 걸어둔다. **따로 반납하지 않는다** - 풀이 죽을 때 같이 간다.
// 실패하면 VK_NULL_HANDLE.
VkDescriptorSet AllocateImageSet(const Descriptors& descriptors, VkImageView view) noexcept;
