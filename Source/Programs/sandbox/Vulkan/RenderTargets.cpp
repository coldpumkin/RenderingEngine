#include "Vulkan/RenderTargets.h"

#include <initializer_list>   // 소멸자의 for (Image* : {...})

// 이미지 + 메모리 + 뷰를 한 번에.
//
// **이제 호출자가 둘이라 함수가 됐다** (색, 뎁스). 한때 뎁스 하나뿐이라
// 스왑체인 생성 루프 안에 인라인으로 있었고, 그때는 그게 맞았다.
// 텍스처가 오면 세 번째 호출자가 되는데, 그때는 usage에 SAMPLED가 붙고
// 업로드 경로가 따라오므로 이 함수를 그대로 쓸 수 있는지 다시 볼 자리다.
//
// aspect가 인자인 이유: 뷰의 subresourceRange는 색이면 COLOR, 뎁스면 DEPTH다.
// usage와 aspect가 서로 맞아야 하는데 컴파일러가 못 잡아준다.
static bool CreateImage2D(const VulkanDevice& dev,
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
    info.samples = VK_SAMPLE_COUNT_1_BIT;   // 파이프라인의 MSAA 설정과 맞아야 한다
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // GPU만 읽고 쓴다. CPU가 볼 일이 없다.
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.priority = 1.0f;   // 렌더 타겟이다. 쫓겨나면 매 프레임 손해다

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

bool CreateRenderTargets(const VulkanDevice& dev, VkExtent2D extent,
                         RenderTargets* out) noexcept {
    out->dev = &dev;
    out->extent = extent;

    // TRANSFER_SRC가 붙는 것이 오프스크린의 표식이다 - 여기에 그린 다음
    // **다른 곳으로 복사해 나간다**. 스왑체인 이미지에는 이게 필요 없었다.
    if (!CreateImage2D(dev, extent, kRenderColorFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &out->color)) {
        return false;
    }

    // 뎁스는 아무 데도 안 나간다. 이 프레임 안에서만 쓰이고 버려진다.
    if (!CreateImage2D(dev, extent, dev.depthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, &out->depth)) {
        return false;
    }
    return true;
}

// 반쯤 만들어진 상태도 견딘다 - vkDestroy~는 VK_NULL_HANDLE에 no-op이다(스펙 보장).
// 그래서 CreateRenderTargets의 중간 실패 경로에 되돌리기 코드가 없다.
RenderTargets::~RenderTargets() {
    if (dev == nullptr) { return; }
    const VulkanDevice& d = *dev;

    for (Image* img : {&color, &depth}) {
        d.table.vkDestroyImageView(d.handle, img->view, nullptr);
        if (img->handle != VK_NULL_HANDLE) {
            vmaDestroyImage(d.allocator, img->handle, img->allocation);
        }
    }
}
