#include "Vulkan/Texture.h"

#include "Vulkan/Barrier.h"
#include "Vulkan/Buffer.h"

#include <cstring>

Texture::~Texture() {
    if (dev == nullptr) { return; }
    DestroyImage(*dev, &image);
}

bool CreateCheckerTexture(const VulkanDevice& dev,
                          const Commands& commands,
                          const Descriptors& descriptors,
                          Texture* out) noexcept {
    Texture& texture = *out;
    texture.dev = &dev;

    // 8x8 칸. 한 칸이 여러 픽셀이면 확대 필터를 안 거쳐 경계가 또렷해서, uv가 맞는지
    // 보는 데는 작을수록 낫다. sampler가 LINEAR라 칸 경계가 부드럽게 번진다.
    constexpr uint32_t kSize = 8;
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_SRGB;

    // **SRGB다.** vertex color와 곱해지는 값이라 render target과 같은 공간이어야 한다.
    // UNORM으로 만들면 shader가 받는 값이 밝아져 곱한 결과가 뜬다.
    uint8_t pixels[kSize * kSize * 4]{};
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const uint8_t v = ((x + y) % 2 == 0) ? 255 : 70;
            uint8_t* p = pixels + (y * kSize + x) * 4;
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    }

    Buffer staging;
    if (!CreateBuffer(dev, sizeof(pixels),
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
    std::memcpy(staging.mapped, pixels, sizeof(pixels));

    // usage 둘이 짝이다 - 복사를 받고(TRANSFER_DST) shader가 읽는다(SAMPLED).
    // 하나만 적으면 만들어지고 나서 검증 레이어가 잡는다.
    if (!CreateImage2D(dev, VkExtent2D{kSize, kSize}, kFormat,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &texture.image)) {
        return false;
    }

    VkCommandBuffer cmd = BeginOneShot(dev, commands);
    if (cmd == VK_NULL_HANDLE) { return false; }

    // 복사를 받을 수 있는 layout으로. srcStage가 TOP_OF_PIPE인 이유는 앞에 기다릴
    // 것이 없어서다 - 이 image는 방금 만들어졌고 아무도 안 건드렸다.
    RecordLayoutTransition(dev.table, cmd, texture.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // bufferRowLength/ImageHeight가 0이면 "빈틈없이 붙어 있다"는 뜻이다. 큰 이미지의
    // 일부만 올릴 때 여기가 0이 아니게 된다.
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{kSize, kSize, 1};
    dev.table.vkCmdCopyBufferToImage(cmd, staging.handle, texture.image.handle,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // shader가 읽는 layout으로. dstStage가 FRAGMENT_SHADER인 이유는 실제로 거기서만
    // 읽기 때문이다 - vertex shader도 읽게 되면 여기가 같이 넓어져야 한다.
    RecordLayoutTransition(dev.table, cmd, texture.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (!EndOneShotAndWait(dev, commands, cmd, "texture upload")) { return false; }

    // 이 set이 가리키는 layout은 위에서 전이시킨 SHADER_READ_ONLY_OPTIMAL이다.
    // AllocateImageSet이 그렇게 적어둔다 - 어긋나면 검증 레이어가 draw에서 잡는다.
    texture.set = AllocateImageSet(descriptors, texture.image.view);
    if (texture.set == VK_NULL_HANDLE) { return false; }

    LOG("[vk] checker texture ready (%ux%u)\n", kSize, kSize);
    return true;
}
