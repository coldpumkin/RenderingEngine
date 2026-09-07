#include "Vulkan/Texture.h"

#include "Vulkan/Barrier.h"
#include "Vulkan/Buffer.h"
#include "Vulkan/Commands.h"

#include <cstring>

bool CheckSampledInput(const TextureDesc& desc, const char* what,
                       bool wantDepth) noexcept {
    const bool isDepth = IsDepthFormat(desc.format);
    if (isDepth != wantDepth) {
        LOG("[vk] the %s is a %s image and a %s one is wanted\n", what,
            isDepth ? "depth" : "colour", wantDepth ? "depth" : "colour");
        return false;
    }
    return true;
}

bool CreateTexture(const VulkanDevice& dev, const TextureDesc& desc,
                   Texture* out) noexcept {
    out->desc = desc;

    // desc goes through untouched. A multisample image that something samples later
    // needs a second, 1-sample Texture beside it, and the pass that owns both makes
    // it -- SAMPLED on a multisample image would be a validation error, not a hint
    // to create anything here.
    if (!CreateImage(dev, desc, &out->image)) {
        return false;
    }

    // The whole image, the way it already is -- every mip and every layer, addressed
    // the way the kind says. Anything that wants less, like a prefilter drawing into
    // one mip, makes its own view from out->image.handle.
    ImageViewDesc viewDesc;
    viewDesc.type = ViewTypeOf(desc.kind);
    return CreateImageView(dev, out->image.handle, desc.format, desc.samples, desc.usage,
                           viewDesc, &out->view);
}

void ResetTexture(Texture* texture) noexcept {
    texture->view = ImageView{};
    texture->image = Image{};
    texture->desc = TextureDesc{};
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

    // What a mip chain needs of the caller, asked before anything is created. Both are
    // the caller's declaration rather than something to work out here: mipLevels says
    // there is a chain, and the usage says the image may be read from while it is built.
    if (desc.mipLevels > 1) {
        if ((desc.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
            LOG("[vk] %u mip levels asked for without TRANSFER_SRC to build them with\n", desc.mipLevels);
            return false;
        }

        // Each level is a filtered copy of the one above it, so the format has to
        // support being filtered. A format that cannot is refused rather than silently
        // given a nearest-filtered chain, which would look like a bad texture.
        VkFormatProperties props{};
        dev.inst->table.vkGetPhysicalDeviceFormatProperties(dev.gpu, desc.format, &props);
        if ((props.optimalTilingFeatures
                 & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
            LOG("[vk] format %d cannot be linearly filtered, so no mip chain\n",
                static_cast<int>(desc.format));
            return false;
        }
    }

    if (!CreateTexture(dev, desc, out)) { return false; }

    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }

    // Level 0 only. Every level below is moved to a writable layout right before it is
    // written, which is what keeps two writes to the same level ordered -- a single
    // transition of the whole image at the start names every level and then nothing
    // stands between it and the blit that writes level i. The synchronization
    // validation layer reports that as a write-after-write hazard, correctly.
    //
    // srcStage is TOP_OF_PIPE because there is nothing to wait for: this image was just
    // created and nobody has touched it.
    RecordLayoutTransition(dev.table, cmd, out->image.handle,
                           OneLayer(VK_IMAGE_ASPECT_COLOR_BIT, 0, 0),
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

    // The chain. Vulkan has no glGenerateMipmap: level i is a filtered copy of level
    // i - 1, so each step moves the source to a readable layout, opens the destination,
    // and blits between them.
    int32_t width = static_cast<int32_t>(desc.extent.width);
    int32_t height = static_cast<int32_t>(desc.extent.height);
    for (uint32_t level = 1; level < desc.mipLevels; ++level) {
        // The level just written becomes the source. Its write was the buffer copy for
        // level 0 and a blit for every level after that.
        RecordLayoutTransition(dev.table, cmd, out->image.handle,
                               OneLayer(VK_IMAGE_ASPECT_COLOR_BIT, 0, level - 1),
                               level == 1 ? VK_PIPELINE_STAGE_2_COPY_BIT
                                          : VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_READ_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        // And this level becomes the destination. From UNDEFINED because nothing has
        // ever been in it, which is also why there is no earlier write to make visible.
        RecordLayoutTransition(dev.table, cmd, out->image.handle,
                               OneLayer(VK_IMAGE_ASPECT_COLOR_BIT, 0, level),
                               VK_PIPELINE_STAGE_2_BLIT_BIT, 0,
                               VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // Halved, and never below one: a 1024 x 16 image reaches 1 in one direction
        // long before the other, and the levels after that are one texel tall.
        const int32_t nextWidth = width > 1 ? width / 2 : 1;
        const int32_t nextHeight = height > 1 ? height / 2 : 1;

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = VkOffset3D{width, height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = VkOffset3D{nextWidth, nextHeight, 1};
        dev.table.vkCmdBlitImage(cmd, out->image.handle,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 out->image.handle,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1, &blit, VK_FILTER_LINEAR);
        width = nextWidth;
        height = nextHeight;
    }

    // Two transitions where there is a chain, because the levels are not in the same
    // layout: every level but the last was read from to make the next, and the last was
    // only written.
    if (desc.mipLevels > 1) {
        const VkImageSubresourceRange read{VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                           desc.mipLevels - 1, 0, 1};
        RecordLayoutTransition(dev.table, cmd, out->image.handle, read,
                               VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_READ_BIT,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // dstStage is FRAGMENT_SHADER because that is the only place we read it. A vertex
    // shader sampling textures would widen this.
    RecordLayoutTransition(dev.table, cmd, out->image.handle,
                           OneLayer(VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mipLevels - 1),
                           desc.mipLevels > 1 ? VK_PIPELINE_STAGE_2_BLIT_BIT
                                              : VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return EndOneShotAndWait(dev, commands, cmd, "texture upload");
}

bool ReadTexturePixels(const VulkanDevice& dev, const Commands& commands,
                       const Texture& texture, VkImageLayout current,
                       std::vector<uint8_t>* out) noexcept {
    const TextureDesc& desc = texture.desc;

    if ((desc.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        LOG("[vk] this texture was not made readable (usage has no TRANSFER_SRC)\n");
        return false;
    }
    if (desc.samples != VK_SAMPLE_COUNT_1_BIT) {
        LOG("[vk] cannot read a multisample image -- read its resolve instead\n");
        return false;
    }
    // The four we actually make. A wider set would need a bytes-per-texel table, and
    // there is nothing yet to put in one.
    if (desc.format != VK_FORMAT_R8G8B8A8_SRGB && desc.format != VK_FORMAT_R8G8B8A8_UNORM
            && desc.format != VK_FORMAT_B8G8R8A8_SRGB
            && desc.format != VK_FORMAT_B8G8R8A8_UNORM) {
        LOG("[vk] cannot read format %d -- four 8-bit channels only\n", desc.format);
        return false;
    }

    const VkDeviceSize size =
        static_cast<VkDeviceSize>(desc.extent.width) * desc.extent.height * 4;

    // A local, so ~Buffer frees it on every exit path below.
    Buffer staging;
    if (!CreateBuffer(dev, size,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                          | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                      &staging)) {
        return false;
    }
    if (staging.mapped == nullptr) {
        LOG("[vk] staging buffer is not mapped (texture read)\n");
        return false;
    }

    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }

    // srcStage covers both ways this image is written: as an attachment, and by the
    // resolve at the end of a pass. Neither is known here, so the barrier waits for
    // the wider one.
    RecordLayoutTransition(dev.table, cmd, texture.image.handle, WholeImage(VK_IMAGE_ASPECT_COLOR_BIT),
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT,
                           current,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{desc.extent.width, desc.extent.height, 1};
    dev.table.vkCmdCopyImageToBuffer(cmd, texture.image.handle,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     staging.handle, 1, &region);

    // Back where it was found. A capture in the middle of a run must leave nothing
    // behind for the next frame's barriers to disagree with.
    RecordLayoutTransition(dev.table, cmd, texture.image.handle, WholeImage(VK_IMAGE_ASPECT_COLOR_BIT),
                           VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           current);

    if (!EndOneShotAndWait(dev, commands, cmd, "texture read")) { return false; }

    // No-op on coherent memory, which this almost certainly is. Called anyway because
    // "almost certainly" is not what the spec says.
    vmaInvalidateAllocation(dev.allocator, staging.allocation, 0, size);

    out->resize(static_cast<size_t>(size));
    std::memcpy(out->data(), staging.mapped, static_cast<size_t>(size));
    return true;
}
