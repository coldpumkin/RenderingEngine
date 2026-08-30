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

#include "Vulkan/Descriptors.h"

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

    // **color를 셰이더가 읽는 손잡이.** 두 번째 패스가 이걸 바인딩한다.
    //
    // 여기 있는 이유: 이 셋은 **위 color.view를 가리킨다.** 따로 들고 다니면 다른
    // 프레임의 이미지를 가리켜도 컴파일된다 - image와 imageIndex를 묶은 것과 같다.
    // 같은 draw에서 "여기 그린다(color.view)"와 "이걸 읽는다(colorSet)"가 나오면
    // 패스 사이의 연결이 코드에 보인다.
    //
    // 풀이 죽을 때 같이 사라지므로 소멸자가 따로 안 지운다.
    VkDescriptorSet colorSet = VK_NULL_HANDLE;

    RenderTargets() = default;
    ~RenderTargets();
    RenderTargets(const RenderTargets&) = delete;
    RenderTargets& operator=(const RenderTargets&) = delete;
};

// **우리 렌더 타겟의 포맷 계약.** 값 둘이 아니라 하나다.
//
// 만드는 쪽(CreateRenderTargets)과 맞추는 쪽(파이프라인)이 **같은 것을 봐야 한다** -
// 다이나믹 렌더링은 포맷을 파이프라인에 박기 때문이다. 따로 넘기면 어긋나도 컴파일된다.
// (image와 imageIndex를 묶은 것과 같은 이유다.)
//
// **왜 Device가 아니라 여기 있나**: 후보 목록과 그 우선순위는 우리 렌더 타겟의
// 정책이지 GPU의 성질이 아니다. GPU는 "지원하나"에만 답한다.
//
// 커지는 자리이기도 하다 - MSAA 샘플 수, HDR 색 포맷, G-buffer의 첨부 여럿.
struct RenderTargetFormats {
    VkFormat color = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
};

// 이 GPU에서 쓸 포맷 한 쌍을 고른다. 실패하면 depth가 VK_FORMAT_UNDEFINED.
// 조회가 인스턴스 레벨이라 inst를 받는다.
RenderTargetFormats ChooseRenderTargetFormats(const VulkanInstance& inst,
                                              VkPhysicalDevice gpu) noexcept;

bool CreateRenderTargets(const VulkanDevice& dev, const Descriptors& descriptors,
                         VkExtent2D extent, RenderTargetFormats formats,
                         RenderTargets* out) noexcept;
