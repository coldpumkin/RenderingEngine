#include "Vulkan/Swapchain.h"

#include "Config.h"
#include "Vulkan/Window.h"

#include <memory>
#include <utility>
#include <vector>

// ============================================================================
VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) noexcept {
    for (const VkSurfaceFormatKHR& f : available) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB
            && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return available.front();   // 스펙상 목록은 최소 하나가 보장된다
}

// OPAQUE = "알파를 무시하고 불투명하게 합성하라". 창 투명도를 안 쓰므로 이게 맞다.
//
// **그래도 확인하고 고른다.** FIFO와 달리 이건 스펙이 지원을 보장하지 않는다.
// 지원 안 하는 값을 박아 넣으면 vkCreateSwapchainKHR이 실패하는데, 그 실패 코드만으로는
// 무엇이 문제인지 알 수 없다.
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

    // GPU가 아직 이 이미지들을 쓰고 있을 수 있다. 스펙상 사용 중인 오브젝트 파괴는 금지다.
    d.table.vkDeviceWaitIdle(d.handle);

    for (SwapchainImage& img : images) {
        d.table.vkDestroySemaphore(d.handle, img.renderFinished, nullptr);
        d.table.vkDestroyImageView(d.handle, img.view, nullptr);
        // img.image는 파괴하지 않는다 - vkGetSwapchainImagesKHR로 **조회**한 것이고
        // 스왑체인이 소유한다.
    }
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

    window->surfaceFormat = ChooseSurfaceFormat(formats);
    return true;
}

// 실패 또는 "지금은 만들 수 없음"(최소화)이면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// oldSwapchain을 넘겨야 하는 이유: **한 서피스에 스왑체인 둘이 동시에 존재할 수 없다.**
// 안 넘기면 재생성 자체가 실패한다. 넘기면 그 순간 이전 것은 "은퇴" 상태가 되고,
// **파괴는 여전히 우리 몫이다.**
//
// **테이블이 둘이다.** 서피스 조회는 인스턴스 레벨(inst.table), 스왑체인 생성은
// 디바이스 레벨(dev.table)이라 양쪽이 다 필요하다 - 스왑체인이 두 층의 경계에 서 있다.
bool CreateSwapchain(const VulkanInstance& inst,
                     const VulkanDevice& dev,
                     VkSurfaceKHR surface,
                     VkSurfaceFormatKHR surfaceFormat,
                     VkSwapchainKHR oldSwapchain,
                     Swapchain* out) noexcept {
    Swapchain& sc = *out;
    sc.dev = &dev;

    // ---- 서피스에게 "이 창은 무엇을 받나"를 묻는다 ----
    VkSurfaceCapabilitiesKHR caps{};
    if (inst.table.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.gpu, surface, &caps) != VK_SUCCESS) {
        LOG("[vk] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed\n");
        return false;
    }

    // 창이 최소화되면 서피스 크기가 0x0이 된다. 스왑체인을 만들 수 없지만 **오류가 아니라
    // 정상 상황**이고, 최소화가 풀릴 때까지 지속된다. 그래서 로그를 찍지 않는다 -
    // 찍으면 최소화하고 있는 내내 초당 수백 줄이 된다.
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
        return false;
    }

    // 포맷은 인자로 받는다 - **여기서 고르지 않는다.** 서피스의 성질이라 창이 들고 있다.
    // (반면 compositeAlpha와 extent는 caps에서 오고 리사이즈마다 달라질 수 있어서 여기다.)

    const VkCompositeAlphaFlagBitsKHR compositeAlpha =
        ChooseCompositeAlpha(caps.supportedCompositeAlpha);
    if (compositeAlpha == 0) {
        LOG("[vk] no usable composite alpha (0x%x)\n", caps.supportedCompositeAlpha);
        return false;
    }

    // 이미지 개수: 최소보다 하나 더 요청한다. 최소만 요청하면 드라이버가 다음 이미지를
    // 내줄 때까지 매번 기다리게 된다. maxImageCount == 0은 "상한 없음"이라 클램프에서 뺀다.
    // 요청값을 하드웨어가 허용하는 범위로 클램프한다.
    // maxImageCount == 0은 "상한 없음"이라 클램프에서 뺀다.
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
    // COLOR_ATTACHMENT만 있으면 된다. **두 번째 패스가 여기에 직접 그린다.**
    //
    // 블릿으로 내보내던 동안은 TRANSFER_DST가 필요했고, 그건 스펙이 보장하지 않아서
    // supportedUsageFlags를 확인해야 했다. 이제 그 확인이 사라졌다 -
    // **COLOR_ATTACHMENT는 스펙이 항상 보장하는 유일한 용도다.**
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // EXCLUSIVE로 두는 이유: **스왑체인 이미지는 그래픽스 큐만 만진다** (그리고, present한다).
    // 컴퓨트/전송 큐가 따로 있어도 이 이미지에는 손대지 않는다. 나중에 컴퓨트가 스왑체인
    // 이미지에 직접 써야 하면 그때 CONCURRENT로 바꾸거나 큐 패밀리 소유권 이전을 넣는다
    // - **둘 다 비용이 있으니 필요해질 때 고른다.**
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = compositeAlpha;
    // FIFO는 **스펙이 항상 지원을 보장하는 유일한 모드**다. 수직동기와 같아 티어링이 없다.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    const VkResult created = dev.table.vkCreateSwapchainKHR(dev.handle, &info, nullptr, &sc.handle);
    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateSwapchainKHR failed (%d)\n", created);
        sc.handle = VK_NULL_HANDLE;
        return false;
    }

    sc.format = surfaceFormat.format;
    sc.extent = caps.currentExtent;

    // ---- 이미지 조회 ----
    // **개수는 요청이 아니라 결과다.** minImageCount는 최소일 뿐이고 드라이버가 더 줄 수
    // 있다. 만든 뒤에 실제 개수를 다시 물어야 한다.
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

    // ---- 이미지마다 뷰 + 세마포어 ----
    //
    // **여기서 실패하면 통째로 버린다.** 반만 만들어진 스왑체인을 성공으로 돌려주면
    // 호출자는 sc.handle이 유효한 것만 보고 다음 프레임에 null 뷰로 렌더링을 시도한다.
    // "객체가 존재하면 항상 유효하다"를 지키려면 중간 실패에서 되돌려야 한다.
    //
    // 되돌리기는 **소멸자가 한다.** 실패하면 호출자가 이 객체를 놓고, ~Swapchain이
    // 반쯤 채워진 배열을 정리한다 - vkDestroy~는 VK_NULL_HANDLE에 no-op이다(스펙 보장).
    sc.images.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        sc.images[i].image = rawImages[i];
        sc.images[i].index = i;

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = rawImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = sc.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        const VkResult viewResult =
            dev.table.vkCreateImageView(dev.handle, &viewInfo, nullptr, &sc.images[i].view);
        if (viewResult != VK_SUCCESS) {
            LOG("[vk] vkCreateImageView failed on image %u (%d)\n", i, viewResult);
                return false;
        }

        // **이미지당 하나인 이유**: 이 세마포어는 present가 기다린다. 그런데 present에는
        // 완료를 알려주는 것이 없다 - vkQueuePresentKHR은 펜스를 주지 않는다.
        // "다시 signal해도 된다"는 유일한 단서가 **acquire가 그 이미지를 다시 줬다**는
        // 사실이고, 그 단서는 이미지 인덱스로만 온다. 그래서 개수도 이미지 수다.
        //
        // (반대로 imageAvailable은 acquire를 부르기 전에는 인덱스를 모르므로 이미지당으로
        //  둘 수 없다. 정확히 반대 방향이고, 이 비대칭이 "스왑체인에 묶인 자원"과
        //  "프레임에 묶인 자원"을 가르는 선이다.)
        //
        // 완전한 해법은 VK_KHR_swapchain_maintenance1의 VkSwapchainPresentFenceInfoKHR다 -
        // present에 펜스를 붙일 수 있다. **그 확장이 따로 생겼다는 사실 자체가
        // 원래 present에 완료 신호가 없었다는 증거다.** 지금은 필요 없다.
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
        static_cast<int>(sc.format));
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

    // 이전 것을 oldSwapchain으로 넘겨 **은퇴시키고**, 새것을 만든 뒤에 놓는다.
    // 넘긴 것은 "은퇴시켜라"는 뜻이지 "네가 지워라"가 아니다 - 파괴는 여전히 우리 몫이다.
    // (생성이 실패해도 은퇴는 일어나므로, 실패해도 이전 것은 버려야 한다.)
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