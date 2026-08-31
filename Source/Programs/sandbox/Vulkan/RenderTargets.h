#pragma once

// Render targets - 우리가 그리는 곳. swapchain과 무관하다.
// ============================================================================
//
// Surface와 swapchain은 optional extension이라 창 없이도 렌더링이 성립한다
// (off-screen rendering). 그래서 render target이 swapchain image를 참조할 수 없다.
// 방향은 반대다: 여기에 그린 다음 결과를 swapchain으로 내보낸다.
//
// Frame이 소유하는 이유: 개수의 근거가 "동시에 그려지는 frame 수" = kFramesInFlight다.
// Swapchain image 수와 상관없다. 한때 depth를 SwapchainImage에 뒀는데 재생성 경로가
// 거기 있어서 편했을 뿐이고, 개수도 3개가 되어 2개면 되는 것을 하나 더 만들었다.
//
// Frame.h가 아니라 따로 있는 이유: 바뀌는 이유가 다르다.
//   semaphore · fence · queue를 바꾸면        -> Frame
//   그릴 곳의 구성을 바꾸면(HDR · MSAA · G-buffer) -> 여기

#include "Vulkan/Descriptors.h"

// 우리가 만들고 우리가 지우는 image 한 장 + view.
// Swapchain image와 반대다 - 그쪽은 조회해서 받고 view만 우리가 만든다.
struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// 한 frame이 그려 넣을 한 벌. kFramesInFlight개 존재한다.
struct RenderTargets {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    Image color;
    Image depth;
    VkExtent2D extent{};

    // 위 color.view를 가리키는 descriptor set. Pass 2가 이걸 bind한다.
    //
    // 여기 있는 이유: 따로 들고 다니면 다른 frame의 image를 가리켜도 컴파일된다.
    // 같은 draw에서 color.view(쓴다)와 colorSet(읽는다)이 나오면 pass 사이의 연결이
    // 코드에 보인다. Pool이 죽을 때 같이 사라지므로 소멸자가 안 지운다.
    VkDescriptorSet colorSet = VK_NULL_HANDLE;

    RenderTargets() = default;
    ~RenderTargets();
    RenderTargets(const RenderTargets&) = delete;
    RenderTargets& operator=(const RenderTargets&) = delete;
};

// Format 계약. 값 둘이 아니라 하나다.
//
// 만드는 쪽(CreateRenderTargets)과 맞추는 쪽(pipeline)이 같은 것을 봐야 한다 -
// dynamic rendering은 format을 pipeline에 굽기 때문이다. 따로 넘기면 어긋나도 컴파일된다.
//
// Device가 아니라 여기 있는 이유: 후보 목록과 우선순위는 우리 render target의
// 정책이지 GPU의 성질이 아니다. GPU는 "지원하나"에만 답한다.
//
// 커지는 자리다 - MSAA sample 수, HDR color format, G-buffer의 attachment 여럿.
struct RenderTargetFormats {
    VkFormat color = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
};

// Input:  inst, gpu
// Output: 이 GPU에서 쓸 format 한 쌍 (실패하면 depth가 VK_FORMAT_UNDEFINED)
//
// 조회가 instance level이라 inst를 받는다. Logical device는 필요 없다.
RenderTargetFormats ChooseRenderTargetFormats(const VulkanInstance& inst,
                                              VkPhysicalDevice gpu) noexcept;

bool CreateRenderTargets(const VulkanDevice& dev, const Descriptors& descriptors,
                         VkExtent2D extent, RenderTargetFormats formats,
                         RenderTargets* out) noexcept;
