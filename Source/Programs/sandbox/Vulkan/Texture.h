#pragma once

// Texture - an image and the set that reads it
// ============================================================================
//
// A set records (view, sampler), which is this pair, so it lives here. Having one
// means the next stage reads this texture; color and depth targets have none.
//
// Two ways to fill one: draw into it (a stage does that) or upload pixels.

#include "Vulkan/Image.h"

struct Commands;

struct Texture {
    Image image;
    VkDescriptorSet set = VK_NULL_HANDLE;   // filled by whoever reads it; the pool frees it
};

// What one texture is. usage is the only field a caller really chooses -- the rest
// comes from AttachmentFormats or from the file the pixels came out of.
struct TextureDesc {
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0;
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
