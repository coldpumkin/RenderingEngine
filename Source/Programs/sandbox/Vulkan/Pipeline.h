#pragma once

#include "Vulkan/Device.h"

// ============================================================================
// 7. 그래픽스 파이프라인 - **무엇으로 그리는가**
// ============================================================================
//
// **파이프라인은 스왑체인 포맷에 묶인다** (VkPipelineRenderingCreateInfo의
// pColorAttachmentFormats). 크기에는 안 묶인다 - 뷰포트/시저를 동적 상태로 뒀다.
// 그래서 리사이즈로는 재생성이 필요 없다.
struct Pipeline {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 비소유 상태

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline handle = VK_NULL_HANDLE;
    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};

// colorFormat: 이 파이프라인이 어떤 포맷의 렌더 타겟에 그릴지. 창에서 온다.
//
// **뎁스 포맷은 인자가 아니다** - dev.depthFormat에서 온다. 색 포맷은 (GPU, 서피스)
// 쌍이 정해서 창이 들고 있지만, 뎁스는 GPU만 보면 정해지므로 디바이스가 안다.
bool CreateTrianglePipeline(const VulkanDevice& dev, VkFormat colorFormat,
                            Pipeline* out) noexcept;