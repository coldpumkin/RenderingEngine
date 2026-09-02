#include "Vulkan/Swapchain.h"

#include "Config.h"
#include "Vulkan/Window.h"

#include <memory>
#include <utility>
#include <vector>

// ============================================================================
// UNDEFINED means none was usable. Any SRGB format will do -- the shader writes and
// reads (r,g,b,a) whatever the byte order is, so only the colour space matters.
VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) noexcept {
    for (const VkSurfaceFormatKHR& f : available) {
        const bool srgb = f.format == VK_FORMAT_B8G8R8A8_SRGB
                       || f.format == VK_FORMAT_R8G8B8A8_SRGB
                       || f.format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
        if (srgb && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { return f; }
    }
    return VkSurfaceFormatKHR{};
}

// OPAQUE = 알파를 무시하고 불투명하게 합성. 창 투명도를 안 쓰므로 이게 맞다.
//
// FIFO와 달리 스펙이 지원을 보장하지 않아 확인하고 고른다. 지원 안 하는 값을 박으면
// vkCreateSwapchainKHR이 실패하는데 그 실패 코드만으로는 원인을 알 수 없다.
VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) noexcept {
    constexpr VkCompositeAlphaFlagBitsKHR kPreferred[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR candidate : kPreferred) {
        if ((supported & candidate) != 0) { return candidate; }
    }
    return static_cast<VkCompositeAlphaFlagBitsKHR>(0);   // 드라이버가 스펙을 어긴 경우
}

Swapchain::~Swapchain() {
    if (handle == VK_NULL_HANDLE || dev == nullptr) { return; }
    const VulkanDevice& d = *dev;

    // 스펙: 사용 중인 object 파괴는 금지다. GPU가 아직 이 image들을 쓸 수 있다.
    d.table.vkDeviceWaitIdle(d.handle);

    for (SwapchainImage& img : images) {
        d.table.vkDestroySemaphore(d.handle, img.renderFinished, nullptr);
    }
    // ~Image가 view를 지운다. image 자체는 allocation이 없어서 건드리지 않는다 -
    // 조회한 것이고 vkDestroySwapchainKHR이 가져간다.
    images.clear();

    d.table.vkDestroySwapchainKHR(d.handle, handle, nullptr);
}

bool SelectSurfaceFormat(const VulkanInstance& inst,
                         VkPhysicalDevice gpu,
                         Window* window) noexcept {
    uint32_t count = 0;
    inst.table.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, window->surface, &count, nullptr);
    if (count == 0) {
        LOG("[vk] surface reports no formats\n");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    inst.table.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, window->surface, &count,
                                                    formats.data());

    // A non-SRGB surface would store what the shader wrote without encoding it, and
    // nothing catches that - the picture is simply wrong. No fallback path, as with 1x.
    const VkSurfaceFormatKHR chosen = ChooseSurfaceFormat(formats);
    if (chosen.format == VK_FORMAT_UNDEFINED) {
        LOG("[vk] surface offers no SRGB format\n");
        return false;
    }
    window->surfaceFormat = chosen;
    return true;
}

// 스펙: 한 surface에 swapchain 둘이 동시에 존재할 수 없다. 그래서 재생성할 때
// oldSwapchain을 넘겨야 하고, 넘기는 순간 이전 것은 retired가 된다(파괴는 여전히
// 우리 몫이다).
//
// Table이 둘인 이유: surface 조회는 instance level, swapchain 생성은 device level이라
// swapchain이 두 층의 경계에 서 있다.
bool CreateSwapchain(const VulkanInstance& inst,
                     const VulkanDevice& dev,
                     VkSurfaceKHR surface,
                     VkSurfaceFormatKHR surfaceFormat,
                     VkSwapchainKHR oldSwapchain,
                     Swapchain* out) noexcept {
    Swapchain& sc = *out;
    sc.dev = &dev;

    VkSurfaceCapabilitiesKHR caps{};
    if (inst.table.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.gpu, surface, &caps) != VK_SUCCESS) {
        LOG("[vk] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed\n");
        return false;
    }

    // 최소화하면 surface 크기가 0x0이 된다. 오류가 아니라 정상 상황이고 최소화가
    // 풀릴 때까지 지속되므로 log를 찍지 않는다 - 찍으면 초당 수백 줄이 된다.
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
        return false;
    }

    const VkCompositeAlphaFlagBitsKHR compositeAlpha =
        ChooseCompositeAlpha(caps.supportedCompositeAlpha);
    if (compositeAlpha == 0) {
        LOG("[vk] no usable composite alpha (0x%x)\n", caps.supportedCompositeAlpha);
        return false;
    }

    // 요청값을 하드웨어가 허용하는 범위로 clamp한다.
    // maxImageCount == 0은 "상한 없음"이라 clamp에서 뺀다.
    uint32_t imageCount = kDesiredSwapchainImages;
    if (imageCount < caps.minImageCount) { imageCount = caps.minImageCount; }
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat.format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = caps.currentExtent;
    info.imageArrayLayers = 1;
    // Present stage가 여기에 직접 그리므로 COLOR_ATTACHMENT만 있으면 된다.
    // 스펙이 supportedUsageFlags에 항상 넣는 유일한 용도라 확인도 필요 없다.
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // 현재 정책: swapchain image는 graphics queue만 만진다. Compute가 여기 직접 써야
    // 하면 CONCURRENT로 바꾸거나 queue family ownership transfer를 넣는다 - 둘 다
    // 비용이 있으니 필요해질 때 고른다.
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = compositeAlpha;
    // FIFO는 스펙이 항상 지원을 보장하는 유일한 mode다. vsync와 같아 tearing이 없다.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    const VkResult created = dev.table.vkCreateSwapchainKHR(dev.handle, &info, nullptr, &sc.handle);
    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateSwapchainKHR failed (%d)\n", created);
        sc.handle = VK_NULL_HANDLE;
        return false;
    }

    sc.extent = caps.currentExtent;

    // 개수는 요청이 아니라 결과다. minImageCount는 최소일 뿐이고 driver가 더 줄 수
    // 있어서 만든 뒤에 다시 물어야 한다.
    uint32_t actualCount = 0;
    if (dev.table.vkGetSwapchainImagesKHR(dev.handle, sc.handle, &actualCount, nullptr)
            != VK_SUCCESS || actualCount == 0) {
        LOG("[vk] vkGetSwapchainImagesKHR returned no images\n");
        return false;
    }
    std::vector<VkImage> rawImages(actualCount);
    if (dev.table.vkGetSwapchainImagesKHR(dev.handle, sc.handle, &actualCount, rawImages.data())
            != VK_SUCCESS) {
        LOG("[vk] vkGetSwapchainImagesKHR failed\n");
        return false;
    }

    // 중간에 실패하면 통째로 버린다. 반만 만들어진 swapchain을 성공으로 돌려주면
    // 호출자는 handle이 유효한 것만 보고 null view로 렌더링을 시도한다.
    //
    // 되돌리기는 소멸자가 한다 - vkDestroy~는 VK_NULL_HANDLE에 no-op이라(스펙 보장)
    // 반쯤 채워진 배열도 그대로 정리된다.
    sc.images.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        // 조회한 image에 우리가 만든 view를 붙인다. desc는 이 image가 무엇인지 -
        // present stage의 pipeline이 같은 format으로 만들어져야 한다.
        Texture& texture = sc.images[i].texture;
        texture.desc = {sc.extent, surfaceFormat.format, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        texture.image.dev = &dev;
        texture.image.handle = rawImages[i];   // allocation은 비운다 = 우리 것이 아니다
        sc.images[i].index = i;

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = rawImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        const VkResult viewResult =
            dev.table.vkCreateImageView(dev.handle, &viewInfo, nullptr, &texture.image.view);
        if (viewResult != VK_SUCCESS) {
            LOG("[vk] vkCreateImageView failed on image %u (%d)\n", i, viewResult);
                return false;
        }

        // image당 하나인 이유: 이 semaphore는 present가 기다리는데 present에는 완료를
        // 알려주는 것이 없다(vkQueuePresentKHR은 fence를 주지 않는다). "다시 signal해도
        // 된다"는 유일한 단서가 "acquire가 그 image를 다시 줬다"이고 그건 image index로만
        // 온다.
        //
        // imageAvailable은 반대다 - acquire 전에는 index를 모르므로 image당으로 둘 수
        // 없다. 이 비대칭이 swapchain에 묶인 자원과 frame에 묶인 자원을 가르는 선이다.
        //
        // 완전한 해법은 VK_KHR_swapchain_maintenance1의 VkSwapchainPresentFenceInfoKHR -
        // present에 fence를 붙일 수 있다. 그 extension이 따로 생겼다는 것 자체가 원래
        // present에 완료 신호가 없었다는 증거다.
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        const VkResult semResult =
            dev.table.vkCreateSemaphore(dev.handle, &semInfo, nullptr,
                                        &sc.images[i].renderFinished);
        if (semResult != VK_SUCCESS) {
            LOG("[vk] vkCreateSemaphore failed on image %u (%d)\n", i, semResult);
                return false;
        }

    }

    LOG("[vk] swapchain %ux%u, %u images (min %u), format %d, FIFO\n",
        sc.extent.width, sc.extent.height, actualCount, caps.minImageCount,
        static_cast<int>(surfaceFormat.format));
    return true;
}

// ---------------------------------------------------------------------------
// 창 단위 연산 - 스왑체인이 있어야 성립하므로 여기(5절 끝)에 있다
// ---------------------------------------------------------------------------

// 그릴 곳을 보장한다. 낡았거나 없으면 다시 만든다.
// **false는 실패가 아니라 "지금은 그릴 곳이 없다"** (최소화 중)이다.
//
// 루프의 0단계가 통째로 여기 들어왔다. 재생성 조건 판단, oldSwapchain 넘기기,
// 이전 것 파괴가 한 덩어리라 흩어져 있을 이유가 없다.
bool EnsureSwapchain(const VulkanDevice& dev, Window* window) noexcept {
    if (!window->swapchainOutOfDate && window->swapchain != nullptr) {
        return true;
    }

    dev.table.vkDeviceWaitIdle(dev.handle);

    // Surface에 format을 다시 묻는다 - 결과가 고정이라는 보장이 없다(Window.h).
    //
    // 여기가 그 재조회의 자리인 이유: format이 바뀌는 사건과 swapchain을 다시 만드는
    // 사건이 같은 것이다. 창이 다른 모니터로 가면 둘 다 일어난다.
    //
    // 실패는 무시한다 - 이전 format으로 계속 가는 것이 그릴 곳이 없어지는 것보다 낫다.
    // 바뀌었다고 알릴 필요는 없다: 새 swapchain image가 자기 format을 들고 가고,
    // pipeline과 다르면 그것이 곧 신호다.
    SelectSurfaceFormat(*window->inst, dev.gpu, window);

    // 이전 것을 oldSwapchain으로 넘겨 retire시키고, 새것을 만든 뒤에 놓는다.
    // 스펙: 생성이 실패해도 retire는 일어난다 - 그래서 실패해도 이전 것은 버려야 한다.
    const VkSwapchainKHR retiring =
        window->swapchain != nullptr ? window->swapchain->handle : VK_NULL_HANDLE;

    auto fresh = std::make_unique<Swapchain>();
    const bool created = CreateSwapchain(*window->inst, dev, window->surface,
                                         window->surfaceFormat, retiring, fresh.get());

    // **새것을 만든 뒤에** 이전 것을 놓는다. reset()이 소멸자를 부른다.
    window->swapchain.reset();

    // 실패하면 fresh가 여기서 파괴된다 - 반쯤 만들어진 것도 소멸자가 정리한다.
    // 그래서 CreateSwapchain의 중간 실패 경로에 되돌리기 코드가 하나도 없다.
    if (created) {
        window->swapchain = std::move(fresh);
    }
    window->swapchainOutOfDate = false;

    return window->swapchain != nullptr;
}