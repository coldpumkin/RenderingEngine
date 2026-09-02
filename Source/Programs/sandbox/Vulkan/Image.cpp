#include "Vulkan/Image.h"

#include <new>   // placement new in move assignment

// Stencil is never used, so a stencil format still gets a depth-only view. Adding
// stencil here would put a second aspect on every barrier and view.
static VkImageAspectFlags AspectOf(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

bool CreateImage2D(const VulkanDevice& dev,
                   VkExtent2D extent,
                   VkFormat format,
                   VkSampleCountFlagBits samples,
                   VkImageUsageFlags usage,
                   Image* out) noexcept {
    out->dev = &dev;   // set first: the destructor runs even if the create below fails

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = VkExtent3D{extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
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
    viewInfo.subresourceRange.aspectMask = AspectOf(format);
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

Image::Image(Image&& other) noexcept
    : dev(other.dev), handle(other.handle), allocation(other.allocation), view(other.view) {
    other.dev = nullptr;
    other.handle = VK_NULL_HANDLE;
    other.allocation = VK_NULL_HANDLE;
    other.view = VK_NULL_HANDLE;
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        this->~Image();
        new (this) Image(static_cast<Image&&>(other));
    }
    return *this;
}

// vkDestroy* is a no-op on VK_NULL_HANDLE by spec, so a failed create needs no unwind.
//
// The view goes either way -- we made it. The image only if we allocated it: a
// swapchain image is queried, and destroying it would take the swapchain's.
Image::~Image() {
    if (dev == nullptr) { return; }
    dev->table.vkDestroyImageView(dev->handle, view, nullptr);
    if (allocation != VK_NULL_HANDLE) {
        vmaDestroyImage(dev->allocator, handle, allocation);
    }
}
