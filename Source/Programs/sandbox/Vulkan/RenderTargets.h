#pragma once

// ============================================================================
// 우리가 그리는 곳 - **스왑체인과 무관하다**
// ============================================================================
//
// Vulkan에서 서피스와 스왑체인은 **선택적 확장**이다. 창을 안 만들고도 렌더링이
// 완전히 성립한다("off-screen rendering"). 그래서 렌더 타겟이 스왑체인 이미지를
// 참조할 수는 없다 - 스왑체인이 아예 없을 수도 있으니까.
//
// 방향은 반대다: 여기에 다 그린 다음 **결과를 스왑체인으로 내보낸다.**
// 내보내기는 표시 단계이지 렌더링 단계가 아니다.
//
// ---------------------------------------------------------------------------
// **왜 Frame이 소유하나** (Swapchain이 아니라):
//
//   개수의 근거가 "동시에 그려지는 프레임 수"다 = kFramesInFlight.
//   스왑체인 이미지 수와는 상관이 없다.
//
//   한때 뎁스 버퍼를 SwapchainImage 안에 뒀었다. 재생성 경로가 이미 거기 있어서
//   편했는데, 그건 **자원의 성질이 아니라 편의**였다. 개수도 3개(이미지 수)가 되어
//   2개면 충분한 것을 하나 더 만들고 있었다.
//
// **왜 Frame.h가 아니라 따로 있나**: 바뀌는 이유가 다르다.
//   동기화를 바꾸면(세마포어 · 펜스 · 큐)      -> Frame
//   그릴 곳의 구성을 바꾸면(HDR · MSAA · G-buffer) -> 여기
// ---------------------------------------------------------------------------

#include "Vulkan/Device.h"

// 우리가 만들고 우리가 지우는 이미지 한 장 + 그 뷰.
//
// 스왑체인 이미지와 정확히 반대다 - 그쪽은 조회해서 받고 뷰만 우리가 만든다.
struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// 한 프레임이 그려 넣을 한 벌. **kFramesInFlight개 존재한다.**
struct RenderTargets {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 비소유 상태

    Image color;
    Image depth;
    VkExtent2D extent{};

    RenderTargets() = default;
    ~RenderTargets();
    RenderTargets(const RenderTargets&) = delete;
    RenderTargets& operator=(const RenderTargets&) = delete;
};

// 색은 항상 이 포맷으로 그린다. **스왑체인 포맷과 독립이다** - 블릿이 변환해준다.
//
// UNORM인 이유: 지금 셰이더가 이미 sRGB 값을 내놓는 것처럼 동작하고 있었다
// (스왑체인이 _SRGB라 하드웨어가 변환해줬다). 오프스크린을 _SRGB로 두면 변환이
// 두 번 일어난다. 나중에 HDR로 갈 때 여기가 R16G16B16A16_SFLOAT가 되는 자리다.
constexpr VkFormat kRenderColorFormat = VK_FORMAT_R8G8B8A8_SRGB;

// 뎁스는 상수로 못 박는다. 스펙이 보장하는 것은 D32_SFLOAT와 X8_D24_UNORM_PACK32 중
// **최소 하나**지 특정 하나가 아니다. 그래서 물어보고 고른다.
//
// **왜 Device가 아니라 여기 있나**: 후보 목록과 그 우선순위는 우리 렌더 타겟의
// 정책이지 GPU의 성질이 아니다. GPU는 "지원하나"에만 답한다.
// (스텐실 없는 것을 먼저 보는 이유도 "우리가 스텐실을 안 쓴다"이지 GPU와 무관하다.)
//
// 실패하면 VK_FORMAT_UNDEFINED. 조회가 인스턴스 레벨이라 inst를 받는다.
VkFormat ChooseDepthFormat(const VulkanInstance& inst, VkPhysicalDevice gpu) noexcept;

bool CreateRenderTargets(const VulkanDevice& dev, VkExtent2D extent,
                         VkFormat depthFormat, RenderTargets* out) noexcept;
