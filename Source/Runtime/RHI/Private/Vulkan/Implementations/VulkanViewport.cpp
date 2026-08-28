#include "Implementations/VulkanViewport.h"

#include "RHILog.h"

#include <utility>

namespace LambdaEngine {

std::unique_ptr<VulkanViewport> VulkanViewport::Create(const VulkanInstance& instance,
                                                        const VulkanDevice& device,
                                                        NativeWindowHandle window) noexcept {
    auto surface = VulkanSurface::Create(instance, window);
    if (surface == nullptr) {
        return nullptr;
    }

    // **이 창에 대한 present 확인은 여기다.** GPU 선택은 "이 플랫폼에 present 되는가"까지만
    // 답할 수 있었다 - 그때는 창이 없었다. 층이 다르지 하나가 다른 하나를 대신하는 게
    // 아니다 (D95). 묻는 것은 서피스다 - 주역이 서피스이므로.
    if (!surface->SupportsPresent(device.PhysicalDevice(), device.GraphicsQueueFamily())) {
        LAMBDA_LOG_ERROR("selected queue family cannot present to this window");
        return nullptr;
    }

    // 스왑체인은 없어도 뷰포트는 유효하다. 최소화된 창으로 시작하면 여기서 nullptr이
    // 나오는데 그건 실패가 아니라 "아직 그릴 곳이 없다"이고, RHI가 다음 프레임에 만든다.
    auto swapchain = VulkanSwapchain::Create(device, *surface);

    if (swapchain != nullptr) {
        LAMBDA_LOG_INFO("viewport ready (%ux%u, %u images)",
                        swapchain->Extent().width, swapchain->Extent().height,
                        swapchain->ImageCount());
    } else {
        LAMBDA_LOG_INFO("viewport ready (no swapchain yet)");
    }

    return std::unique_ptr<VulkanViewport>(
        new VulkanViewport(std::move(surface), std::move(swapchain)));
}

VulkanViewport::VulkanViewport(std::unique_ptr<VulkanSurface> surface,
                               std::unique_ptr<VulkanSwapchain> swapchain) noexcept
    : surface_(std::move(surface)), swapchain_(std::move(swapchain)) {}

} // namespace LambdaEngine
