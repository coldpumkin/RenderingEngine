#pragma once

#include "Vulkan/Descriptors.h"
#include "Vulkan/RenderTargets.h"

// Graphics pipeline - 무엇으로 그리는가
// ============================================================================
//
// Dynamic rendering이 attachment format을 pipeline에 굽는다. 크기에는 안 묶인다 -
// viewport/scissor를 dynamic state로 뒀으므로 리사이즈로 재생성이 필요 없다.
struct Pipeline {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline handle = VK_NULL_HANDLE;

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
struct PushConstants {
    float time;     // 초. glfwGetTime()
    float aspect;   // width / height. 없으면 리사이즈할 때 도형이 늘어난다
};

// Scene pass용. Vertex buffer를 읽고 depth test를 한다.
// Contract: formats가 RenderTargets가 실제로 만든 것과 같아야 한다.
bool CreateTrianglePipeline(const VulkanDevice& dev,
                            RenderTargetFormats formats,
                            Pipeline* out) noexcept;

// Present pass용. Pass 1의 결과를 texture로 읽어 swapchain에 그린다.
//
// Triangle pipeline과 다른 점이 계약에서 드러난다:
//   colorFormat  swapchain format이다 (우리 render target format이 아니다)
//   depth        없다
//   vertex input 없다 - shader가 gl_VertexIndex로 세 점을 만든다
//   setLayout    있다 - image를 읽으므로 pipeline layout이 비지 않는다
bool CreateFullscreenPipeline(const VulkanDevice& dev,
                              VkFormat colorFormat,
                              VkDescriptorSetLayout setLayout,
                              Pipeline* out) noexcept;
