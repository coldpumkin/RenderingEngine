#pragma once

// Texture - one attachment
// ============================================================================
//
// One Texture is one attachment. Vulkan agrees: VkRenderingAttachmentInfo holds the
// multisample view and its resolve view together, so resolve is a field of this
// attachment, not a second one.
//
// desc decides the resolve, so the two cannot disagree:
//   samples > 1 and SAMPLED  -> resolve exists, and the stage averages into it
//
// No descriptor set here: a set belongs to the stage that binds it, not to one of the
// things it names -- it can name several.
//
// Two ways to fill one: draw into it (a stage does that) or upload pixels.

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
    Image resolve;   // only when desc.samples > 1
};

// Which view the next stage samples. A multisample image cannot be read through
// sampler2D, so it is the resolve when there is one.
inline VkImageView ReadView(const Texture& texture) noexcept {
    return texture.resolve.view != VK_NULL_HANDLE ? texture.resolve.view
                                                  : texture.image.view;
}

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
