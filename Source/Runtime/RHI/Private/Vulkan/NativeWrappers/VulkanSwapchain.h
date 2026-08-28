#pragma once

#include "NativeWrappers/VulkanDevice.h"
#include "NativeWrappers/VulkanSurface.h"

#include <volk.h>

#include <memory>
#include <vector>

namespace LambdaEngine {

// 스왑체인 이미지 한 장에 딸린 것 전부.
//
// 셋을 병렬 벡터로 들면 **개수가 어긋나도 컴파일된다.** 인덱스 하나로 함께 지목되는
// 값들이므로 한 몸으로 둔다 - "이 실수를 컴파일러가 잡아줄 수 있나"에 예인 자리다.
struct SwapchainImage {
    VkImage image = VK_NULL_HANDLE;               // 조회한 것. 우리가 파괴하지 않는다
    VkImageView view = VK_NULL_HANDLE;            // 우리가 만들었다 -> 우리가 파괴한다
    VkSemaphore renderFinished = VK_NULL_HANDLE;  // 우리가 만들었다. 이미지당 하나
};

// 화면에 내보낼 이미지 묶음. 서피스가 답해준 규격에 맞춰 이미지를 만들어 들고 있다.
//
// 서피스와 나눈 이유(D24): 서피스는 창이 사는 동안 유지되지만 스왑체인은 창 크기가
// 바뀔 때마다 다시 만든다.
class VulkanSwapchain {
public:
    // surface를 참조로 받는 이유: 능력·포맷을 **서피스에게 물어야** 하기 때문이다
    // (surface.QuerySupport). 값 하나가 아니라 그 객체에게 시키는 일이다 (기준 B).
    // oldSwapchain: 재생성 시 이전 스왑체인. 없으면 VK_NULL_HANDLE.
    //
    // 넘겨야 하는 이유: 한 서피스에 스왑체인 둘이 동시에 존재할 수 없다. 새것을 만든 뒤
    // 갈아끼우려면(실패 시 돌아갈 곳을 남기려면) 이전 것이 살아 있어야 하고, 그러려면
    // 이걸로 "저건 은퇴시킬 것"이라고 알려줘야 한다. 안 넘기면 생성 자체가 실패한다.
    // 덤으로 드라이버가 이전 자원을 재활용할 수 있다.
    static std::unique_ptr<VulkanSwapchain> Create(const VulkanDevice& device,
                                                   const VulkanSurface& surface,
                                                   VkSwapchainKHR oldSwapchain
                                                       = VK_NULL_HANDLE) noexcept;

    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;
    VulkanSwapchain(VulkanSwapchain&&) = delete;
    VulkanSwapchain& operator=(VulkanSwapchain&&) = delete;

    VkSwapchainKHR Handle() const noexcept { return swapchain_; }
    VkFormat ImageFormat() const noexcept { return format_; }
    VkExtent2D Extent() const noexcept { return extent_; }
    uint32_t ImageCount() const noexcept { return static_cast<uint32_t>(images_.size()); }

    // 인덱스는 vkAcquireNextImageKHR이 돌려준 값이다. 그 인덱스로 지목되는 것이
    // 셋이므로 한 번에 준다.
    const SwapchainImage& ImageAt(uint32_t index) const noexcept { return images_[index]; }

private:
    VulkanSwapchain(const VulkanDevice& device,
                    VkSwapchainKHR swapchain,
                    VkFormat format,
                    VkExtent2D extent,
                    std::vector<SwapchainImage> images) noexcept;

    const VulkanDevice& device_;   // 파괴에 필요한 비소유 상태(②)

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;

    // 이미지 한 장 + 그 이미지의 뷰 + 그 이미지의 완료 세마포어.
    //
    // 세마포어가 여기 있는 이유(기준 A ③): 개수가 스왑체인 이미지 수로 정해지고,
    // 불변이고, 스왑체인이 죽으면 의미를 잃는다. 한때 프레임 자원 쪽에 있었는데
    // 그건 "남의 자원이 정하는 값"이라 기준 A 위반이었다.
    std::vector<SwapchainImage> images_;

    // 이 스왑체인의 정체(③). extent_는 "창의 크기"가 아니라 "이 스왑체인이 만들어진
    // 크기"다. 창은 그새 커졌을 수 있어 서피스에 다시 물어도 이 값을 얻을 수 없다.
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
};

} // namespace LambdaEngine
