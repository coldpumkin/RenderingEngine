#include "Vulkan/Texture.h"

#include "Vulkan/Barrier.h"
#include "Vulkan/Buffer.h"
#include "Vulkan/Commands.h"

#include <cstring>

bool CreateTexture(const VulkanDevice& dev, const TextureDesc& desc,
                   Texture* out) noexcept {
    out->desc = desc;

    // desc goes through untouched. A multisample image that something samples later
    // needs a second, 1-sample Texture beside it, and the pass that owns both makes
    // it -- SAMPLED on a multisample image would be a validation error, not a hint
    // to create anything here.
    if (!CreateImage2D(dev, desc.extent, desc.format, desc.samples, desc.usage,
                       &out->image)) {
        return false;
    }

    // {}: the whole image, the way it already is. Anything that wants less makes its
    // own view from out->image.handle.
    return CreateImageView(dev, out->image.handle, desc.format, {}, &out->view);
}

bool CreateTextureFromPixels(const VulkanDevice& dev, const Commands& commands,
                             const TextureDesc& desc,
                             const void* pixels, VkDeviceSize size,
                             Texture* out) noexcept {
    // A local, so ~Buffer frees it on every exit path below.
    Buffer staging;
    if (!CreateBuffer(dev, size,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                          | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                      &staging)) {
        return false;
    }
    if (staging.mapped == nullptr) {
        LOG("[vk] staging buffer is not mapped (texture)\n");
        return false;
    }
    std::memcpy(staging.mapped, pixels, size);

    if (!CreateTexture(dev, desc, out)) { return false; }

    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }

    // srcStage is TOP_OF_PIPE because there is nothing to wait for: this image was
    // just created and nobody has touched it.
    RecordLayoutTransition(dev.table, cmd, out->image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // bufferRowLength/ImageHeight of 0 means tightly packed. They stop being 0 once
    // only part of a larger image is uploaded.
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{desc.extent.width, desc.extent.height, 1};
    dev.table.vkCmdCopyBufferToImage(cmd, staging.handle, out->image.handle,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // dstStage is FRAGMENT_SHADER because that is the only place we read it. A vertex
    // shader sampling textures would widen this.
    RecordLayoutTransition(dev.table, cmd, out->image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return EndOneShotAndWait(dev, commands, cmd, "texture upload");
}
