#pragma once

#include "DynamicRHI.h"
#include "Implementations/VulkanCommandContext.h"
#include "NativeWrappers/VulkanDevice.h"
#include "NativeWrappers/VulkanInstance.h"

#include <memory>

namespace LambdaEngine {

// 재생성 대상. 참조 인자로만 쓰므로 정의가 필요 없다.
class VulkanViewport;

// DynamicRHI의 유일한 구현체. 상위 코드는 이 이름을 모른다.
//
// **여기 있는 것은 GPU backend뿐이다** (D95). 창에 붙는 것은 전부 VulkanViewport가 가진다 -
// 이 클래스는 창이 하나도 없어도, 창이 다 닫혀도 유효하다.
//
// 프레임을 여닫는 것도 여기다. **기록만 `RHICommandContext`로 나가고**, 동기화(펜스 대기 ·
// acquire · present)는 전부 이 안에서 끝난다 - 상위는 그것들의 존재를 모른다.
class VulkanRHI final : public DynamicRHI {
public:
    // GPU backend만 만든다. 창은 모른다.
    static std::unique_ptr<VulkanRHI> Create() noexcept;

    // = default다. 여기가 드는 것 중 GPU 작업을 기다려야 하는 것은 `context_`뿐인데
    // **그건 자기 소멸자에서 스스로 기다린다.** 소유자가 대신 챙기지 않는다.
    ~VulkanRHI() override = default;

    std::unique_ptr<RHIViewport> CreateViewport(NativeWindowHandle window) noexcept override;
    void ResizeViewport(RHIViewport& viewport) noexcept override;

    RHICommandContext* BeginDrawingViewport(RHIViewport& viewport) noexcept override;
    void EndDrawingViewport(RHIViewport& viewport) noexcept override;

    VulkanRHI(const VulkanRHI&) = delete;
    VulkanRHI& operator=(const VulkanRHI&) = delete;
    VulkanRHI(VulkanRHI&&) = delete;
    VulkanRHI& operator=(VulkanRHI&&) = delete;

private:
    VulkanRHI(std::unique_ptr<VulkanInstance> instance,
              std::unique_ptr<VulkanDevice> device) noexcept;

    // 그릴 준비. 커맨드 풀(디바이스가 소유)과 기록기(여기가 소유)를 만든다.
    //
    // **`Create()`에 없는 이유**: 커맨드 풀도 기록기도 없이 RHI는 유효하다 - "이 기계에서
    // GPU 작업을 할 수 있는가"(D95)와 무관하고 **그릴 때 필요한 것**이다 (D96).
    // 그래서 처음 그릴 때 만든다. 판별은 한 줄이다: **"그것 없이도 이 객체가 유효한가?"**
    //
    // 2단계 초기화(D14)가 아니다. D14가 막는 것은 "생성됐지만 아직 못 쓰는 객체"인데,
    // RHI는 이것 없이도 뷰포트를 만들 수 있고 그게 정상 상태다.
    bool EnsureDrawingResources() noexcept;

    // 선언 순서 = 생성 순서. 파괴는 역순이다 (D90).
    //
    // **GPU backend는 인스턴스와 디바이스가 전부다.** 창에 붙는 것(서피스·스왑체인)은
    // 뷰포트가 가져갔다.
    //
    // `context_`가 마지막인 것은 디바이스보다 먼저 죽어야 해서다 - 펜스와 세마포어를
    // 디바이스 레벨 함수로 파괴한다.
    std::unique_ptr<VulkanInstance> instance_;
    std::unique_ptr<VulkanDevice> device_;
    std::unique_ptr<VulkanCommandContext> context_;
};

} // namespace LambdaEngine
