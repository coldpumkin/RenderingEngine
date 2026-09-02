#pragma once

// Image - one image we create and destroy, plus its view
// ============================================================================
//
// The opposite of a swapchain image, which is queried and lends us only its view.
//
// It owns itself, like Buffer: a half-built one still frees, and holding several
// costs the holder no destructor.
//
// allocation says what we own. A swapchain image is queried, so it has none and only
// the view -- which is ours -- is destroyed.

#include "Vulkan/Device.h"

struct Image {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    Image() = default;
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    // Movable because a swapchain keeps its images in a vector. The source is left
    // empty, so its destructor frees nothing.
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
};

// Input:  samples는 MSAA sample 수 (1_BIT면 MSAA 없음)
//         usage는 무엇에 쓸 image인가 (attachment / sampled / 복사 대상)
// Output: image + allocation + view가 채워진 Image
//
// The view's aspect comes from format, so a depth image cannot get a color view.
//
// Contract: samples가 이 image를 attachment로 쓰는 pipeline의 rasterizationSamples와
//           같아야 한다. 검증 레이어가 잡아준다 - vkCmdBeginRendering에서 말한다.
//
// 기본값을 안 준 이유: 1_BIT가 기본이면 MSAA image를 만들 자리에서 깜빡해도
// 컴파일된다. 호출자가 셋뿐이라 명시가 싸다.
bool CreateImage2D(const VulkanDevice& dev,
                   VkExtent2D extent,
                   VkFormat format,
                   VkSampleCountFlagBits samples,
                   VkImageUsageFlags usage,
                   Image* out) noexcept;
