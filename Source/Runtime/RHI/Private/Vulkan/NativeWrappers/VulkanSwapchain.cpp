#include "NativeWrappers/VulkanSwapchain.h"

#include "RHILog.h"
#include "VulkanResult.h"

#include <algorithm>

namespace LambdaEngine {
namespace {

// 색 포맷 고르기.
//
// SRGB를 우선하는 이유: 모니터는 선형이 아니라 감마 곡선으로 빛을 낸다.
// 포맷에 _SRGB가 붙어 있으면 GPU가 쓰기/읽기 때 변환을 하드웨어로 해준다.
// UNORM을 쓰면 셰이더에서 직접 감마 보정을 해야 하고, 안 하면 화면이 어둡게 나온다.
//
// 못 찾으면 첫 번째를 쓴다. 스펙상 목록은 최소 하나가 보장된다.
VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) noexcept {
    for (const VkSurfaceFormatKHR& format : available) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB
            && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return available.front();
}

// 프레젠트 모드 고르기.
//
// FIFO는 **스펙이 항상 지원을 보장하는 유일한 모드**다. 수직동기와 같아서
// 티어링이 없고 프레임을 모니터 주사율에 맞춘다. 지금은 이걸로 충분하다.
//
// MAILBOX(최신 프레임으로 갈아치우기, 저지연)나 IMMEDIATE(즉시, 티어링 있음)는
// 실제로 지연이나 프레임레이트가 문제가 될 때 고른다. 지금 고르면 근거가 없다.
VkPresentModeKHR ChoosePresentMode() noexcept {
    return VK_PRESENT_MODE_FIFO_KHR;
}

// 창 합성 방식 고르기.
//
// OPAQUE는 "알파를 무시하고 불투명하게 합성하라"는 뜻이다. 창 투명도를 안 쓰므로 이게
// 맞고, 데스크톱에서는 사실상 항상 지원된다.
//
// **그래도 확인하고 고른다.** FIFO와 달리 이건 **스펙이 지원을 보장하지 않는다.**
// 지원 안 하는 값을 박아 넣으면 vkCreateSwapchainKHR이 실패하는데, 그 실패 코드만으로는
// 무엇이 문제인지 알 수 없다. "이 값이 왜 이건지 답할 수 있어야 한다"(D39)에 걸린다.
//
// 하나도 못 찾으면 0을 반환한다. 스펙상 supportedCompositeAlpha는 최소 한 비트를 가지므로
// 도달할 수 없지만, **도달했다면 드라이버가 스펙을 어긴 것**이고 그건 환경 문제라
// 호출자가 실패로 처리한다 (D16).
VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) noexcept {
    // 선호 순서: 불투명 → 창 시스템이 알파를 미리 곱함 → 우리가 곱함 → 부모에서 상속
    constexpr VkCompositeAlphaFlagBitsKHR kPreferred[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };

    for (VkCompositeAlphaFlagBitsKHR candidate : kPreferred) {
        if ((supported & candidate) != 0) {
            return candidate;
        }
    }
    return static_cast<VkCompositeAlphaFlagBitsKHR>(0);
}

} // namespace

std::unique_ptr<VulkanSwapchain> VulkanSwapchain::Create(const VulkanDevice& device,
                                                         const VulkanSurface& surface,
                                                         VkSwapchainKHR oldSwapchain) noexcept {
    const VolkDeviceTable& table = device.Table();

    // ---- 1. 서피스에게 "이 창은 무엇을 받나"를 묻는다 ----
    // **묻는 것은 서피스의 일이고 여기는 소비하는 쪽이다** - 답을 받아서 규격을 정한다.
    const SurfaceSupport support = surface.QuerySupport(device.PhysicalDevice());
    if (!support.valid) {
        return nullptr; // 실패 이유는 QuerySupport가 이미 기록했다
    }

    const VkSurfaceCapabilitiesKHR& caps = support.capabilities;

    // currentExtent가 특수값(0xFFFFFFFF)이면 "창 시스템이 크기를 안 정해줬으니
    // 네가 골라라"라는 뜻이다. Win32에서는 항상 실제 창 크기가 들어오므로 여기 안 걸린다.
    // 다른 플랫폼(일부 Wayland 컴포지터)으로 갈 때 처리가 필요해진다 - 그때 창 크기를
    // 인자로 받게 된다. 지금 미리 만들면 검증할 수 없는 경로를 만드는 것이다.
    if (caps.currentExtent.width == UINT32_MAX) {
        LAMBDA_LOG_ERROR("surface has no fixed extent; not handled on this platform");
        return nullptr;
    }

    // 창이 최소화되면 서피스 크기가 0x0이 된다. 스왑체인을 만들 수 없지만 **오류가 아니라
    // 정상 상황**이고, 최소화가 풀릴 때까지 지속된다. 그래서 로그를 찍지 않는다 -
    // 찍으면 최소화하고 있는 내내 초당 수백 줄이 된다 (D65).
    //
    // 호출자는 여기서 온 nullptr을 "실패"가 아니라 "아직 그릴 곳이 없다"로 읽는다.
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
        return nullptr;
    }

    const VkExtent2D extent = caps.currentExtent;

    // 이미지 개수: 최소보다 하나 더 요청한다.
    // 최소만 요청하면 드라이버가 다음 이미지를 내줄 때까지 매번 기다리게 된다.
    // maxImageCount == 0은 "상한 없음"이라는 뜻이라 클램프에서 빼야 한다.
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    // ---- 2. 서피스가 준 목록 안에서 고르기 ----
    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(support.formats);

    const VkCompositeAlphaFlagBitsKHR compositeAlpha =
        ChooseCompositeAlpha(caps.supportedCompositeAlpha);
    if (compositeAlpha == 0) {
        LAMBDA_LOG_ERROR("surface supports no known composite alpha mode (0x%x)",
                         caps.supportedCompositeAlpha);
        return nullptr; // 아직 아무것도 안 만들었으므로 되돌릴 것이 없다
    }

    // ---- 3. 스왑체인 생성 ----
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface.Handle();
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat.format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;                                   // VR(눈 2개)이 아니면 1

    // 여기에 직접 그린다. **확인하지 않는 것은 빠뜨린 게 아니다** - 스펙이
    // supportedUsageFlags에 COLOR_ATTACHMENT_BIT를 항상 포함하도록 보장한다.
    // 다른 용도(후처리를 위한 TRANSFER_DST 등)를 더하는 순간부터는 확인해야 한다.
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    // 그래픽스와 프레젠트가 같은 큐 패밀리라 EXCLUSIVE로 둔다 (D45).
    // 패밀리가 나뉘면 CONCURRENT로 바꾸거나 이미지 소유권을 명시적으로 넘겨야 하는데,
    // EXCLUSIVE가 더 빠르다 - 드라이버가 동기화를 덜 해도 되기 때문.
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

    info.preTransform = caps.currentTransform;                   // 회전 없음(모바일용 기능)
    info.compositeAlpha = compositeAlpha;                        // 지원 목록에서 고른 것
    info.presentMode = ChoosePresentMode();
    info.clipped = VK_TRUE;                                      // 가려진 픽셀은 안 그려도 됨
    info.oldSwapchain = oldSwapchain;                            // 재생성 시 이전 것을 은퇴시킨다

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult result = table.vkCreateSwapchainKHR(device.Handle(), &info, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
        LAMBDA_LOG_ERROR("vkCreateSwapchainKHR failed: %s", ToString(result));
        return nullptr;
    }

    // ---- 4. 이미지 꺼내오기 ----
    // 만드는 게 아니라 **이미 만들어진 것을 조회**한다 (vkGetDeviceQueue와 같은 성질).
    // 그래서 우리가 파괴하지 않는다. minImageCount는 "최소"라서 실제 개수는 더 많을 수 있다.
    uint32_t actualCount = 0;
    table.vkGetSwapchainImagesKHR(device.Handle(), swapchain, &actualCount, nullptr);
    std::vector<VkImage> rawImages(actualCount);
    table.vkGetSwapchainImagesKHR(device.Handle(), swapchain, &actualCount, rawImages.data());

    // ---- 5. 이미지마다 뷰와 완료 세마포어를 만든다 ----
    // 뷰는 "이 이미지를 어떤 포맷으로, 어느 부분을 볼 것인가"다. 다이나믹 렌더링에서
    // 렌더 타겟을 지정할 때 이미지가 아니라 뷰를 넘긴다.
    // 세마포어는 개수가 이미지 수에 묶이므로 스왑체인의 것이다 (기준 A ③).
    std::vector<SwapchainImage> images;
    images.reserve(actualCount);   // 아래에서 참조를 잡으므로 재할당이 없어야 한다

    // 정적 팩토리라 생성자에 닿기 전에는 RAII가 작동하지 않는다.
    // 중간에 실패하면 여기까지 만든 것을 손으로 되돌린다 - **한 벡터에 모아두면
    // 되돌리기도 한 루프다.** 아직 안 만든 자리는 VK_NULL_HANDLE인데, Vulkan의
    // vkDestroy~는 VK_NULL_HANDLE을 받으면 아무 일도 하지 않으므로 그냥 넘겨도 된다.
    const auto rollback = [&]() noexcept {
        for (const SwapchainImage& made : images) {
            table.vkDestroySemaphore(device.Handle(), made.renderFinished, nullptr);
            table.vkDestroyImageView(device.Handle(), made.view, nullptr);
        }
        table.vkDestroySwapchainKHR(device.Handle(), swapchain, nullptr);
    };

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    for (uint32_t i = 0; i < actualCount; ++i) {
        SwapchainImage& entry = images.emplace_back();
        entry.image = rawImages[i];

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = entry.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        const VkResult viewResult =
            table.vkCreateImageView(device.Handle(), &viewInfo, nullptr, &entry.view);
        if (viewResult != VK_SUCCESS) {
            LAMBDA_LOG_ERROR("vkCreateImageView[%u] failed: %s", i, ToString(viewResult));
            rollback();
            return nullptr;
        }

        const VkResult semaphoreResult =
            table.vkCreateSemaphore(device.Handle(), &semaphoreInfo, nullptr,
                                    &entry.renderFinished);
        if (semaphoreResult != VK_SUCCESS) {
            LAMBDA_LOG_ERROR("vkCreateSemaphore(renderFinished[%u]) failed: %s", i,
                             ToString(semaphoreResult));
            rollback();
            return nullptr;
        }
    }

    LAMBDA_LOG_INFO("swapchain: %ux%u, %u images, format %d, FIFO",
                    extent.width, extent.height, actualCount,
                    static_cast<int>(surfaceFormat.format));

    return std::unique_ptr<VulkanSwapchain>(
        new VulkanSwapchain(device, swapchain, surfaceFormat.format, extent,
                            std::move(images)));
}

VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device,
                                 VkSwapchainKHR swapchain,
                                 VkFormat format,
                                 VkExtent2D extent,
                                 std::vector<SwapchainImage> images) noexcept
    : device_(device),
      swapchain_(swapchain),
      images_(std::move(images)),
      format_(format),
      extent_(extent) {}

VulkanSwapchain::~VulkanSwapchain() {
    // 이미지와 세마포어를 GPU가 아직 쓰고 있을 수 있다. **자기 자원은 자기가 안전하게
    // 파괴한다** - 소유자가 대신 기다려주기를 기대하지 않는다.
    device_.WaitIdle();

    const VolkDeviceTable& table = device_.Table();

    // 뷰가 이미지를 참조하므로 뷰가 먼저다.
    // image는 건드리지 않는다 - 스왑체인이 소유하므로 아래에서 같이 사라진다.
    for (const SwapchainImage& entry : images_) {
        table.vkDestroySemaphore(device_.Handle(), entry.renderFinished, nullptr);
        table.vkDestroyImageView(device_.Handle(), entry.view, nullptr);
    }

    table.vkDestroySwapchainKHR(device_.Handle(), swapchain_, nullptr);

    LAMBDA_LOG_INFO("swapchain destroyed");
}

} // namespace LambdaEngine
