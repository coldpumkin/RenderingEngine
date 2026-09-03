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
    return true;
}

bool CreateImageView(const VulkanDevice& dev,
                     VkImage image,
                     VkFormat imageFormat,
                     const ImageViewDesc& desc,
                     ImageView* out) noexcept {
    out->dev = &dev;   // set first: the destructor runs even if the create below fails
    out->desc = desc;

    // The two "take it from the image" defaults are resolved here rather than stored
    // that way, so desc keeps saying what the caller asked for.
    const VkFormat format = desc.format != VK_FORMAT_UNDEFINED ? desc.format : imageFormat;
    const VkImageAspectFlags aspect = desc.aspect != 0 ? desc.aspect : AspectOf(format);

    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = desc.type;
    info.format = format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.baseMipLevel = desc.baseMip;
    info.subresourceRange.levelCount = desc.mipCount;
    info.subresourceRange.baseArrayLayer = desc.baseLayer;
    info.subresourceRange.layerCount = desc.layerCount;

    const VkResult created =
        dev.table.vkCreateImageView(dev.handle, &info, nullptr, &out->handle);
    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateImageView failed (%d)\n", created);
        return false;
    }
    return true;
}

Image::Image(Image&& other) noexcept
    : dev(other.dev), handle(other.handle), allocation(other.allocation) {
    other.dev = nullptr;
    other.handle = VK_NULL_HANDLE;
    other.allocation = VK_NULL_HANDLE;
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
// allocation is the whole test: a swapchain image is queried, and destroying it would
// take the swapchain's.
Image::~Image() {
    if (dev == nullptr) { return; }
    if (allocation != VK_NULL_HANDLE) {
        vmaDestroyImage(dev->allocator, handle, allocation);
    }
}

ImageView::ImageView(ImageView&& other) noexcept
    : dev(other.dev), handle(other.handle), desc(other.desc) {
    other.dev = nullptr;
    other.handle = VK_NULL_HANDLE;
}

ImageView& ImageView::operator=(ImageView&& other) noexcept {
    if (this != &other) {
        this->~ImageView();
        new (this) ImageView(static_cast<ImageView&&>(other));
    }
    return *this;
}

// No condition: a view is ours even when the image under it is not.
ImageView::~ImageView() {
    if (dev == nullptr) { return; }
    dev->table.vkDestroyImageView(dev->handle, handle, nullptr);
}
