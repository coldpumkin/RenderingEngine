#pragma once

#include "Vulkan/Descriptors.h"
#include "Vulkan/RenderTargets.h"

// PushConstants가 mat4를 들고 있어서 필요하다. 헤더 온리라 링크에 영향이 없다.
#include <glm/glm.hpp>

// Viewport의 y 방향 - 한 결정이 두 곳에 나타난다
// ============================================================================
//
// Vulkan framebuffer는 y가 아래로 향한다. viewport height를 음수로 주면 shader 쪽이
// y-up이 되는데(VK_KHR_maintenance1, 1.1 core), **그 순간 winding 판정도 같이
// 뒤집힌다** - front/back은 framebuffer 좌표에서 계산한 넓이의 부호로 정해지고,
// y에 음수 배율이 걸리면 그 부호가 반대가 되기 때문이다.
//
// 그래서 viewport의 부호를 정하면 frontFace도 정해진다. 둘을 따로 적으면 어긋나도
// culling이 꺼져 있는 동안은 아무 일도 안 일어나고, 켜는 순간 화면이 빈다.
//
// Contract: 두 shader 모두 clip 좌표에서 shoelace 넓이가 양수인 삼각형을 낸다.
//           그 전제 위에서만 아래 유도가 성립한다. 세어본 값:
//             triangle.vert   world 좌표의 두 삼각형이 각각 +1.0
//             fullscreen.vert (-1,-1) (3,-1) (-1,3) -> +8
//
// **아래 매핑은 추론이 아니라 실측이다.** 넓이 부호와 enum 이름이 어느 쪽으로
// 대응하는지 스펙을 안 열고 추론했다가 반대로 짚어 화면이 통째로 검게 나왔다
// (두 pipeline이 동시에 culling됐다). 둘 다 CULL_MODE_BACK으로 켜둔 채
// check.ps1이 기준선 픽셀 비율을 되찾는 것으로 확정했다.
enum class ViewportY {
    Down,   // Vulkan 기본. height가 양수
    Up,     // height가 음수. world 좌표가 y-up이라는 우리 규약
};

// Input:  이 pipeline을 그릴 때 쓸 viewport의 y 방향
// Output: 그 방향에서 위 Contract의 삼각형이 앞면이 되는 frontFace
constexpr VkFrontFace FrontFaceFor(ViewportY y) noexcept {
    return y == ViewportY::Up ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                              : VK_FRONT_FACE_CLOCKWISE;
}

// Input:  extent, 그리고 그릴 pipeline의 viewportY
// Output: 부호까지 맞춰진 viewport (기록할 때 vkCmdSetViewport에 그대로 넘긴다)
//
// 호출자가 부호를 손으로 안 적는 것이 요점이다 - pipeline이 든 값을 그대로 넘기면
// 위의 frontFace와 어긋날 수가 없다.
VkViewport MakeViewport(VkExtent2D extent, ViewportY y) noexcept;

// Graphics pipeline - 무엇으로 그리는가
// ============================================================================
//
// Dynamic rendering이 attachment format을 pipeline에 박는다. 크기에는 안 묶인다 -
// viewport/scissor를 dynamic state로 뒀으므로 리사이즈로 재생성이 필요 없다.
struct Pipeline {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline handle = VK_NULL_HANDLE;

    // frontFace가 이 값에서 유도돼 pipeline에 들어가 있다. 기록할 때 MakeViewport에
    // 그대로 넘기라고 여기 남긴다 - 짝이 맞아야 하는 반대편이다.
    ViewportY viewportY = ViewportY::Down;

    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};

// Shader에 매 frame 넘기는 값. Command buffer에 값이 그대로 실려 가므로 pool도
// set도 갱신도 수명 관리도 없다. 스펙이 최소 128바이트를 보장한다.
//
// Contract: shader의 layout(push_constant) 블록과 필드 순서·타입이 같아야 한다.
//           어긋나면 컴파일도 실행도 되는데 값만 이상해진다 - validation layer가
//           크기는 보지만 필드 순서는 못 본다.
// **셋을 CPU에서 곱해 하나로 보낸다.** mat4 셋을 따로 보내면 192바이트라 위의 128
// 보장을 넘는다. 그리고 shader가 셋을 각각 알아야 할 이유가 아직 없다 - 조명이
// world 좌표를 필요로 하게 되면 그때 model이 갈라져 나온다.
//
// glm::mat4는 64바이트 column-major이고 GLSL의 mat4와 레이아웃이 같다. 전치도
// 변환도 없이 그대로 실린다 (`ThirdParty/glm/VERSION.md`가 숫자로 확인해뒀다).
struct PushConstants {
    glm::mat4 mvp;   // model -> world -> view -> clip
};

// Scene pass용. Vertex buffer를 읽고 depth test를 한다. viewportY = Up.
//
// polygonMode를 인자로 받는 유일한 항목인 이유: **호출자에게 실제로 고를 것이
// 있다.** 같은 정점을 면으로 그릴지 선으로 그릴지는 우리가 정할 수 없다.
// 나머지(vertex layout · push 크기 · y-up · culling)는 scene pass의 규약이라
// 호출자가 고를 수 없고, 그래서 안에서 정한다.
//
// Contract: formats가 RenderTargets가 실제로 만든 것과 같아야 한다.
//           LINE은 device의 fillModeNonSolid를 요구한다 (Core.h).
bool CreateTrianglePipeline(const VulkanDevice& dev,
                            RenderTargetFormats formats,
                            VkPolygonMode polygonMode,
                            Pipeline* out) noexcept;

// Present pass용. Pass 1의 결과를 texture로 읽어 swapchain에 그린다.
//
// Triangle pipeline과 다른 점이 계약에서 드러난다:
//   colorFormat  swapchain format이다 (우리 render target format이 아니다)
//   depth        없다
//   vertex input 없다 - shader가 gl_VertexIndex로 세 점을 만든다
//   setLayout    있다 - image를 읽으므로 pipeline layout이 비지 않는다
//   viewportY    Down이다 - shader가 uv를 직접 만들어 쓰므로 뒤집으면 안 된다
bool CreateFullscreenPipeline(const VulkanDevice& dev,
                              VkFormat colorFormat,
                              VkDescriptorSetLayout setLayout,
                              Pipeline* out) noexcept;
