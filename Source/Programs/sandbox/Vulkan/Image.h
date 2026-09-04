#pragma once

// Image, ImageView - the allocation, and how one looks at it
// ============================================================================
//
// Two types because Vulkan has two objects, and because they are owned differently:
//
//   Image      the bytes and their shape. A swapchain's is queried, so it may not be
//              ours to free -- allocation says which
//   ImageView  which part of that, read as what. Always ours, even over a queried
//              image, so its destructor has no condition to check
//
// Everything that draws or reads takes a view. Only a barrier takes the image,
// because a layout transition is a fact about the memory:
//
//   VkRenderingAttachmentInfo.imageView   drawing into it
//   VkDescriptorImageInfo.imageView       reading from it
//   VkImageMemoryBarrier2.image           moving it between those two
//
// One view per image today, held by Texture beside its Image. Nothing here says one:
// mips, cube faces or a depth/stencil split each make it several, and no caller that
// takes a view changes when they do.

#include "Vulkan/Device.h"

struct Image {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;   // null = queried, not ours to free

    Image() = default;
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    // Movable because a swapchain keeps its images in a vector. The source is left
    // empty, so its destructor frees nothing.
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
};

// Which part of an image, seen as what. Every default means "all of it, the way the
// image already is", so a caller with nothing to say passes {}.
//
// Kept by the view because Vulkan cannot be asked what a view sees.
struct ImageViewDesc {
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;   // UNDEFINED = the image's own format
    VkImageAspectFlags aspect = 0;           // 0 = derived from that format

    uint32_t baseMip = 0;
    uint32_t mipCount = VK_REMAINING_MIP_LEVELS;
    uint32_t baseLayer = 0;
    uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS;
};

// No pointer back to its Image. A view is used through the caller that already holds
// both, and a Texture moves inside a vector -- a pointer at a sibling member would
// survive the move pointing at the old one, with nothing to catch it.
struct ImageView {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkImageView handle = VK_NULL_HANDLE;
    ImageViewDesc desc;

    ImageView() = default;
    ~ImageView();
    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;
    ImageView(ImageView&& other) noexcept;
    ImageView& operator=(ImageView&& other) noexcept;
};

// Output: the optimal-tiling format features an image with this usage needs
//
// One usage bit to one feature bit, which is Vulkan's own mapping. It exists so the
// question "can this GPU do that" is asked from the usage a caller already wrote
// down. It used to be one hardcoded pair of bits in QueryTargetCapabilities, which is
// a guess made away from the image: it missed the shadow map's SAMPLED and the
// resolve's TRANSFER_SRC, and demanded SAMPLED of a multisample colour image that
// cannot have it.
VkFormatFeatureFlags RequiredFormatFeatures(VkImageUsageFlags usage) noexcept;

// Input:  samples is the MSAA sample count (1_BIT means no MSAA)
//         usage is what this image is for (attachment / sampled / copy destination)
// Output: an Image with no view. CreateImageView makes those. false also means the
//         format cannot do what usage asks of it, checked here because this is where
//         both are known.
//
// Contract: samples must equal the rasterizationSamples of every pipeline that draws
//           into this. The validation layer says so at vkCmdBeginRendering.
//
// No default for samples: 1_BIT as one would compile at a call site that meant to
// make a multisample image. There are three callers, so being explicit is cheap.
bool CreateImage2D(const VulkanDevice& dev,
                   VkExtent2D extent,
                   VkFormat format,
                   VkSampleCountFlagBits samples,
                   VkImageUsageFlags usage,
                   Image* out) noexcept;

// Input:  imageFormat is what the image was created with -- desc.format UNDEFINED
//         means that one, and desc.aspect 0 is derived from it.
//
// The format is passed rather than read back because Image does not keep it: the
// caller that made the image has it, and a queried swapchain image was told it.
//
// Contract: image must outlive the view. Vulkan destroys neither for the other.
bool CreateImageView(const VulkanDevice& dev,
                     VkImage image,
                     VkFormat imageFormat,
                     const ImageViewDesc& desc,
                     ImageView* out) noexcept;
