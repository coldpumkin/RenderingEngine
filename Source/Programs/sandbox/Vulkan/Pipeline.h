#pragma once

#include "Vulkan/Descriptors.h"
#include "Vulkan/RenderTargets.h"

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

// **셰이더에 매 프레임 넘기는 값.** 여기가 파이프라인 레이아웃이 처음으로 비지 않게
// 되는 지점이다 - 전까지는 셰이더가 정점 버퍼 말고 아무것도 못 받았고, 그래서 뭘
// 시도하려면 정점 데이터를 고쳐 재빌드해야 했다.
//
// **왜 디스크립터가 아니라 푸시 상수인가**: 풀도 셋도 갱신도 수명 관리도 없다.
// 커맨드 버퍼에 값이 그대로 실려 간다. 스펙이 최소 128바이트를 보장하므로
// 행렬 하나 정도는 바로 되고, 그보다 커지거나 이미지를 넘겨야 할 때 디스크립터가 온다.
//
// **레이아웃이 셰이더의 것과 정확히 같아야 한다.** 어긋나면 컴파일도 실행도 되는데
// 값만 이상해진다 - 검증 레이어가 크기는 보지만 필드 순서는 못 본다.
struct PushConstants {
    float time;     // 초. glfwGetTime()
    float aspect;   // width / height. **없으면 리사이즈할 때 도형이 늘어난다**
};

// **RenderTargets가 실제로 만든 것과 같은 계약을 받는다.** 다이나믹 렌더링은 포맷을
// 파이프라인에 박으므로 어긋나면 렌더링 시점에 실패한다.
bool CreateTrianglePipeline(const VulkanDevice& dev,
                            RenderTargetFormats formats,
                            Pipeline* out) noexcept;

// 전체화면 패스용. **첫 패스의 결과를 텍스처로 읽어 스왑체인에 그린다.**
//
// 삼각형 파이프라인과 다른 점이 계약에서 드러난다:
//   colorFormat   스왑체인 포맷이다 (우리 렌더 타겟 포맷이 아니다)
//   뎁스          없다 - 전체화면 사각형에 깊이는 의미가 없다
//   정점 입력     없다 - 셰이더가 gl_VertexIndex로 세 점을 만든다
//   setLayout     있다 - 이미지를 읽으므로 파이프라인 레이아웃이 비지 않는다
//
// **일부러 CreateTrianglePipeline을 복제해서 썼다.** 무엇이 실제로 공통인지는
// 둘을 나란히 놓고 봐야 알 수 있고, 미리 추측해서 인자로 빼면 근거 없는 인터페이스가 된다.
bool CreateFullscreenPipeline(const VulkanDevice& dev,
                              VkFormat colorFormat,
                              VkDescriptorSetLayout setLayout,
                              Pipeline* out) noexcept;