#pragma once

#include "RHIViewport.h"

#include "NativeWindowHandle.h"
#include "NativeWrappers/VulkanDevice.h"
#include "NativeWrappers/VulkanInstance.h"
#include "NativeWrappers/VulkanSurface.h"
#include "NativeWrappers/VulkanSwapchain.h"

#include <cstdint>
#include <memory>
#include <utility>

namespace LambdaEngine {

// RHIViewport의 Vulkan 구현. **창 하나에 그리는 대상**이다.
//
//   서피스     창과 함께 산다. 창이 죽을 때까지 그대로다
//   스왑체인   창 크기가 바뀌면 다시 만든다. **없을 수도 있다**(최소화 중)
//
// 스왑체인이 nullptr인 것은 실패가 아니라 "지금은 그릴 곳이 없다"는 정상 상태다.
//
// ---------------------------------------------------------------------------
// **동작이 없다. 자기 자원을 들고 내줄 뿐이다.**
//
// 리소스는 *무엇인가*를 나타내지 *무엇을 하는가*가 아니다 (D82). 한때 여기에
// "필요하면 스왑체인을 다시 만들고 지금 쓸 수 있는 것을 내주는" 함수가 있었는데,
// 그건 **그리는 쪽의 절차**를 그릴 대상이 대신 밟아준 것이었다.
//
// 재생성은 `VulkanRHI`가 한다. 이 클래스는 그 결과를 받아 든다.
// ---------------------------------------------------------------------------
//
// 디바이스를 들지 않는 이유: 재생성을 안 하므로 필요 없고, 파괴에도 필요 없다 -
// **서피스와 스왑체인이 각자 자기 파괴를 완결한다**(스왑체인은 소멸자에서 GPU까지
// 기다린다). 그래서 소멸자가 `= default`다.
class VulkanViewport final : public RHIViewport {
public:
    // instance와 device는 서피스·스왑체인을 만드는 데만 쓰고 들지 않는다.
    static std::unique_ptr<VulkanViewport> Create(const VulkanInstance& instance,
                                                  const VulkanDevice& device,
                                                  NativeWindowHandle window) noexcept;

    ~VulkanViewport() override = default;

    // --- 여기부터는 RHI 모듈 안에서만 보인다 (D57: 은닉의 경계는 클래스가 아니라 모듈) ---
    // 전부 상태 접근자다. 시키는 것은 하나도 없다.

    // 창과 함께 산다. 재생성할 때 규격을 여기에 묻는다.
    const VulkanSurface& Surface() const noexcept { return *surface_; }

    // **nullptr일 수 있다.** 호출자가 반드시 확인해야 하므로 참조가 아니라 포인터다.
    VulkanSwapchain* Swapchain() const noexcept { return swapchain_.get(); }

    // 재생성 결과를 받는다. nullptr을 넣으면 "그릴 곳이 없음"이 된다.
    void SetSwapchain(std::unique_ptr<VulkanSwapchain> swapchain) noexcept {
        swapchain_ = std::move(swapchain);
    }

    // 지금 스왑체인이 더 이상 창과 맞지 않는다는 표시. 출처는 둘이다 -
    // 상위의 리사이즈 통보와 present 결과(`VK_SUBOPTIMAL_KHR`).
    // **둘 다 결과가 같으므로 플래그 하나다.**
    bool IsOutOfDate() const noexcept { return outOfDate_; }
    void MarkOutOfDate() noexcept { outOfDate_ = true; }
    void ClearOutOfDate() noexcept { outOfDate_ = false; }

    // 지금 그리고 있는 스왑체인 이미지. acquire가 정하고 present가 쓴다.
    //
    // **한때 "프레임 안에서만 사는 값이라 어느 객체의 멤버도 될 수 없다"고 적었는데
    // 그건 acquire와 present가 같은 함수 안에 있을 때의 이야기였다.** 둘이 다른 함수로
    // 갈라진 지금은 그 사이를 건널 곳이 필요하고, 그 값은 "이 뷰포트가 지금 어느 이미지에
    // 그리는 중인가"이므로 뷰포트의 상태다. 언리얼도 `FVulkanViewport::AcquiredImageIndex`로
    // 같은 자리에 둔다.
    //
    // BeginDrawingViewport()와 EndDrawingViewport() 사이에서만 의미가 있다.
    uint32_t AcquiredIndex() const noexcept { return acquiredIndex_; }
    void SetAcquiredIndex(uint32_t index) noexcept { acquiredIndex_ = index; }

private:
    VulkanViewport(std::unique_ptr<VulkanSurface> surface,
                   std::unique_ptr<VulkanSwapchain> swapchain) noexcept;

    // 선언 순서 = 생성 순서. 파괴는 역순이라 스왑체인이 서피스보다 먼저 죽는다 -
    // 스펙상 서피스를 파괴하기 전에 그 서피스로 만든 스왑체인이 없어야 한다. (D90)
    std::unique_ptr<VulkanSurface> surface_;
    std::unique_ptr<VulkanSwapchain> swapchain_;

    bool outOfDate_ = false;

    // 프레임마다 바뀐다. 기준 A ③("자원이 사는 동안 불변")에 걸리는 유일한 멤버이고,
    // 그건 이 값이 자원의 정체가 아니라 **진행 중인 프레임의 상태**이기 때문이다.
    uint32_t acquiredIndex_ = 0;
};

} // namespace LambdaEngine
