#pragma once

// Texture - one GPU image resource
// ============================================================================
//
// One Texture is one image. Being an attachment or a sampled input is not a property
// of the type: desc.usage and the current layout decide that. Unreal's FRHITexture is
// the same shape, and an attachment there (FColorEntry) points at two of them rather
// than nesting one inside the other.
//
// So a resolve target is its own Texture, and the pass that owns both names them.
// Holding it in here gave every sampled texture a field it never used, and turned
// "which view do I read" into a question about a value (is resolve.view null?)
// instead of a question the caller already knows the answer to.
//
// No descriptor set here: a set belongs to the pass that binds it, not to one of the
// things it names -- it can name several.
//
// Two ways to fill one: draw into it (a pass does that) or upload pixels.

#include "Vulkan/Image.h"

struct Commands;

// What one texture is. usage is the only field a caller really chooses -- the rest
// comes from AttachmentFormats or from the file the pixels came out of.
struct TextureDesc {
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0;
};

struct Texture {
    TextureDesc desc;
    Image image;
};

// Output: an empty texture. Something has to draw into it before it is worth reading.
bool CreateTexture(const VulkanDevice& dev, const TextureDesc& desc,
                   Texture* out) noexcept;

// Effect: uploads pixels through a staging buffer and leaves the image
//         SHADER_READ_ONLY_OPTIMAL, which is what a set records.
//
// Contract: usage must include TRANSFER_DST and SAMPLED, and size must match
//           extent x format. Neither is checked here.
bool CreateTextureFromPixels(const VulkanDevice& dev, const Commands& commands,
                             const TextureDesc& desc,
                             const void* pixels, VkDeviceSize size,
                             Texture* out) noexcept;
