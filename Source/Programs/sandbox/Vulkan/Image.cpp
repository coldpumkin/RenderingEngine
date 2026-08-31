#include "Vulkan/Image.h"

bool CreateImage2D(const VulkanDevice& dev,
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

void DestroyImage(const VulkanDevice& dev, Image* image) noexcept {
    dev.table.vkDestroyImageView(dev.handle, image->view, nullptr);
    if (image->handle != VK_NULL_HANDLE) {
        vmaDestroyImage(dev.allocator, image->handle, image->allocation);
    }
    *image = Image{};
}
