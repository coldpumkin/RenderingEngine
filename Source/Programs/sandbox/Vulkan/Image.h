#pragma once

// Image - 우리가 만들고 우리가 지우는 image 한 장 + view
// ============================================================================
//
// Swapchain image와 반대다 - 그쪽은 조회해서 받고 view만 우리가 만든다.
//
// RenderTargets 안에 있던 것이 갈라져 나왔다. **소비자가 둘이 됐기 때문이다** -
// render target(그리는 곳)과 texture(읽는 곳)가 같은 생성 절차를 쓰는데 바뀌는
// 이유는 서로 다르다.
//
// 소유권은 이 구조체가 안 진다. 담고만 있고 파괴는 소유자가 DestroyImage로 한다 -
// RenderTargets는 둘을, Texture는 하나를 들고 각자 자기 소멸자에서 부른다.

#include "Vulkan/Device.h"

struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// Input:  usage는 무엇에 쓸 image인가 (attachment / sampled / 복사 대상)
//         aspect는 view가 어느 면을 보나 (COLOR / DEPTH)
// Output: image + allocation + view가 채워진 Image
bool CreateImage2D(const VulkanDevice& dev,
                   VkExtent2D extent,
                   VkFormat format,
                   VkImageUsageFlags usage,
                   VkImageAspectFlags aspect,
                   Image* out) noexcept;

// 빈 것(VK_NULL_HANDLE)을 줘도 안전하다 - 생성 실패 경로가 그대로 지나간다.
void DestroyImage(const VulkanDevice& dev, Image* image) noexcept;
