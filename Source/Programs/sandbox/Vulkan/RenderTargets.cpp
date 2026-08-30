#include "Vulkan/RenderTargets.h"

#include <initializer_list>   // 소멸자의 for (Image* : {...})

// Image + memory + view를 한 번에.
//
// 호출자가 둘(color, depth)이라 함수가 됐다. Texture가 오면 세 번째가 되는데 usage에
// SAMPLED가 붙고 upload 경로가 따라오므로 그때 이 함수를 그대로 쓸 수 있는지 다시 본다.
//
// Contract: usage와 aspect가 서로 맞아야 하는데 컴파일러가 못 잡는다.
static bool CreateImage2D(const VulkanDevice& dev,
                          VkExtent2D extent,
                          VkFormat format,
                          VkImageUsageFlags usage,
                          VkImageAspectFlags aspect,
                          Image* out) noexcept {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = VkExtent3D{extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;   // pipeline의 MSAA 설정과 맞아야 한다
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // GPU만 읽고 쓴다. priority 1.0 - render target이라 쫓겨나면 매 frame 손해다.
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.priority = 1.0f;

    const VkResult created =
        vmaCreateImage(dev.allocator, &info, &alloc, &out->handle, &out->allocation, nullptr);
    if (created != VK_SUCCESS) {
        LOG("[vk] vmaCreateImage failed (%d)\n", created);
        return false;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = out->handle;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    const VkResult viewed =
        dev.table.vkCreateImageView(dev.handle, &viewInfo, nullptr, &out->view);
    if (viewed != VK_SUCCESS) {
        LOG("[vk] vkCreateImageView failed (%d)\n", viewed);
        return false;
    }
    return true;
}

// Swapchain format과 독립이다 - present pass가 매개한다. HDR로 갈 때 여기가
// R16G16B16A16_SFLOAT가 되는 자리다.
//
// Header에 안 내놓는다. 공개돼 있으면 RenderTargetFormats를 우회해 직접 읽게 되고,
// 실제로 그래서 "color는 직접 읽고 depth는 인자로 받는" 비대칭이 생겼었다.
static constexpr VkFormat kRenderColorFormat = VK_FORMAT_R8G8B8A8_SRGB;

RenderTargetFormats ChooseRenderTargetFormats(const VulkanInstance& inst,
                                              VkPhysicalDevice gpu) noexcept {
    RenderTargetFormats formats;
    formats.color = kRenderColorFormat;

    // 정밀도 높은 순서. Stencil 없는 것을 먼저 보는 이유는 우리가 stencil을 안 쓰기
    // 때문이다 - 붙어 있으면 메모리를 더 쓰고 barrier/view의 aspectMask에 STENCIL까지
    // 얹어야 해서 실수할 자리가 는다. optimalTilingFeatures를 보는 이유는 render target을
    // linear로 두지 않기 때문이다.
    for (const VkFormat candidate : {VK_FORMAT_D32_SFLOAT,
                                     VK_FORMAT_X8_D24_UNORM_PACK32,
                                     VK_FORMAT_D32_SFLOAT_S8_UINT,
                                     VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties props{};
        inst.table.vkGetPhysicalDeviceFormatProperties(gpu, candidate, &props);
        if ((props.optimalTilingFeatures
             & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            formats.depth = candidate;
            break;
        }
    }
    return formats;
}

bool CreateRenderTargets(const VulkanDevice& dev, const Descriptors& descriptors,
                         VkExtent2D extent, RenderTargetFormats formats,
                         RenderTargets* out) noexcept {
    out->dev = &dev;
    out->extent = extent;

    // SAMPLED가 붙는 것이 off-screen의 표식이다 - present pass가 이걸 texture로 읽는다.
    if (!CreateImage2D(dev, extent, formats.color,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &out->color)) {
        return false;
    }

    // Depth는 아무 데도 안 나간다. 이 frame 안에서만 쓰이고 버려진다.
    if (!CreateImage2D(dev, extent, formats.depth,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, &out->depth)) {
        return false;
    }

    // Color image가 생긴 뒤에야 그것을 가리키는 set을 만들 수 있다.
    out->colorSet = AllocateImageSet(descriptors, out->color.view);
    if (out->colorSet == VK_NULL_HANDLE) { return false; }
    return true;
}

// 반쯤 만들어진 상태도 견딘다 - vkDestroy~는 VK_NULL_HANDLE에 no-op이다(스펙 보장).
// 그래서 CreateRenderTargets에 되돌리기 코드가 없다.
RenderTargets::~RenderTargets() {
    if (dev == nullptr) { return; }
    const VulkanDevice& d = *dev;

    for (Image* img : {&color, &depth}) {
        d.table.vkDestroyImageView(d.handle, img->view, nullptr);
        if (img->handle != VK_NULL_HANDLE) {
            vmaDestroyImage(d.allocator, img->handle, img->allocation);
        }
    }
}
