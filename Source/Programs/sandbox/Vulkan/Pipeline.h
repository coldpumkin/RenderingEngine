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

// colorFormat: 이 파이프라인이 어떤 포맷의 렌더 타겟에 그릴지. 스왑체인에서 온다.
// colorFormat: 이 파이프라인이 어떤 포맷의 렌더 타겟에 그릴지. 스왑체인에서 온다.
bool CreateTrianglePipeline(const VulkanDevice& dev, VkFormat colorFormat,
                            Pipeline* out) noexcept;