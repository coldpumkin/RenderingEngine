// **흐름이 있는 곳.** 무엇을 어떤 순서로 만들고, 한 프레임이 어떻게 도는가.
//
// 각 자원을 **어떻게** 만들고 부수는지는 전부 Vulkan/ 아래에 있다. 여기는 순서만 있다.
//
// Vulkan/ 은 **개념당 .h/.cpp 한 쌍**이다:
//   Core.h  함수 테이블 · 요구사항 · RAII 규약
//   Instance  Device  Window  Swapchain  Commands
//   Frame  RenderTargets  Descriptors  Pipeline  Buffer  Barrier
// 자원이 늘어도 기존 파일이 안 자란다 - 새 쌍이 하나 생길 뿐이다.
//
// **초기화와 런타임은 지켜야 할 규칙이 다르다:**
//
//                 초기화        런타임 (매 프레임)
//   실행 횟수     1회           초당 수백
//   힙 할당       마음껏        금지
//   로깅          자유롭게      지속 조건이면 폭주한다
//   실패 처리     되돌린다      프레임을 버리거나 복구
//
// 그게 파일을 나눈 기준이다.

#include "Config.h"
#include "Vulkan/Barrier.h"
#include "Vulkan/Buffer.h"
#include "Vulkan/Commands.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Window.h"

#include <GLFW/glfw3.h>

// 8. 한 프레임 기록하기
// ============================================================================

// ---------------------------------------------------------------------------
// **여기 나오는 것들이 서로 어떤 사이인가** - 셋으로 갈린다.
//
//   서로 맞아야 하는 것 (계약)   포맷 · 파이프라인 · 정점 버퍼
//   명령이 적히는 곳 (동사)      커맨드 버퍼
//   동시에 여러 개 돌리려는 것   프레임
//
// **커맨드 버퍼와 프레임은 "무엇을 그리나"에 아무 말도 안 한다.** 앞은 명령을 적는
// 테이프고, 뒤는 그 한 벌을 kFramesInFlight개 둬서 CPU가 앞서가게 하는 장치다.
//
// 계약은 둘이고 파이프라인이 그 가운데에 있다:
//   파이프라인 <-> 렌더 타겟   **포맷** (다이나믹 렌더링이 포맷을 파이프라인에 굽는다)
//   파이프라인 <-> 정점 버퍼   **정점 레이아웃** (VkVertexInputAttributeDescription <-> Vertex)
//
// **패스는 vkCmdBeginRendering ~ vkCmdEndRendering 구간**이고, 그동안 그릴 대상이
// 고정된다. 한 프레임에 패스가 둘이고, **뒤가 앞의 결과를 읽는다:**
//
//   [RecordScenePass]  창을 모른다
//     배리어 x2 (우리 색·뎁스)
//     +- BeginRendering --- 첨부 = draw.color / draw.depth
//     |    BindPipeline        <- 포맷이 첨부와 같아야 한다
//     |    BindVertexBuffers   <- 레이아웃이 파이프라인과 같아야 한다
//     |    PushConstants       <- 계약이 아니라 그냥 값
//     |    Draw
//     +- EndRendering
//
//   [RecordPresentPass]  draw를 **읽기만** 한다
//     배리어 (draw.color -> SHADER_READ_ONLY)   앞 패스의 결과를 읽을 수 있게
//     배리어 (스왑체인   -> COLOR_ATTACHMENT)
//     +- BeginRendering --- 첨부 = 스왑체인 이미지
//     |    BindPipeline        <- 이쪽 포맷은 **스왑체인 것**이다
//     |    BindDescriptorSets  <- draw.colorSet = draw.color를 가리킨다
//     |    Draw                   정점 버퍼 없음 (셰이더가 세 점을 만든다)
//     +- EndRendering
//     배리어 (스왑체인 -> PRESENT_SRC)
// ---------------------------------------------------------------------------
//
// **동기화(펜스·세마포어·acquire·present)가 하나도 안 들어온다** - 그건 전부
// BeginFrame/SubmitFrame/PresentFrame에 있다. 기록과 동기화는 서로 모르는 채로 돌아간다.
// 이게 "기록하는 쪽"과 "제출하는 쪽"이 갈리는 선이다.
//
// **bool인 이유**: vkBegin/EndCommandBuffer는 실패할 수 있고(메모리 부족), 실패하면
// 커맨드 버퍼가 무효 상태다. 그걸 제출하는 것은 스펙 위반이라 호출자가 알아야 한다.
// ---- 패스 1: 우리 이미지에 장면을 그린다 ----
//
// **스왑체인이 인자에 없다.** 창이 없어도 이 함수는 성립한다.
static void RecordScenePass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                            const RenderTargets& draw,
                            const Pipeline& pipeline,
                            const Buffer& vertexBuffer) noexcept {
    const VkExtent2D extent = draw.extent;   // **창 크기가 아니다.** Config.h가 정한다
    // ---- 그릴 수 있는 레이아웃으로 ----
    // oldLayout이 UNDEFINED인 것은 이전 내용을 안 쓰기 때문이다 - 어차피 loadOp=CLEAR로
    // 덮는다. 보존을 요구하면 드라이버가 실제로 복사를 해야 한다.
    RecordLayoutTransition(vk, cmd, draw.color.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // ---- 뎁스도 쓸 수 있는 레이아웃으로 ----
    //
    // **스테이지가 색과 다르다.** 뎁스 테스트는 프래그먼트 셰이더 앞뒤(EARLY/LATE
    // FRAGMENT_TESTS)에서 일어나고, 색 쓰기(COLOR_ATTACHMENT_OUTPUT)보다 앞이다.
    // 색 배리어의 스테이지를 그대로 쓰면 뎁스 쓰기가 배리어보다 먼저 일어날 수 있다.
    //
    // oldLayout이 UNDEFINED인 것은 색과 같은 이유다 - loadOp=CLEAR로 어차피 덮는다.
    RecordLayoutTransition(vk, cmd, draw.depth.handle, VK_IMAGE_ASPECT_DEPTH_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                               | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    // ---- 렌더링 시작 ----
    // 다이나믹 렌더링: VkRenderPass/VkFramebuffer 객체를 미리 만들지 않는다.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = draw.color.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    // **뎁스 클리어 값은 1.0** - "가장 멈". 파이프라인의 compareOp=LESS와 짝이다.
    // 0.0으로 클리어하면 아무것도 통과하지 못해 화면이 빈다.
    //
    // storeOp가 DONT_CARE인 이유: 뎁스는 이 프레임 안에서만 쓰인다. 다음 프레임에
    // 필요하면(SSAO 같은 후처리) STORE로 바꿔야 한다.
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = draw.depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil.depth = 1.0f;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;

    vk.vkCmdBeginRendering(cmd, &rendering);

    // ======== 드로우 ========
    //
    // 뷰포트와 시저를 여기서 준다. 파이프라인에 박지 않고 동적 상태로 둔 덕에
    // 창 크기가 바뀌어도 파이프라인을 다시 만들 필요가 없다.
    //
    // **y를 뒤집는다**: Vulkan의 클립 좌표는 y가 아래로 향한다(OpenGL과 반대).
    // height를 음수로 주고 y를 아래에서 시작하면 셰이더 좌표계가 위로 향하게 된다.
    // (VK_KHR_maintenance1이 1.1에서 코어가 되면서 가능해진 방법이다.)
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(extent.height);
    viewport.width = static_cast<float>(extent.width);
    viewport.height = -static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);

    // 시저: 이 사각형 밖의 픽셀은 버린다. 지금은 화면 전체다.
    VkRect2D scissor{};
    scissor.extent = extent;
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);

    // **매 프레임 셰이더에 값을 밀어 넣는다.** 커맨드 버퍼에 값이 그대로 실려 가므로
    // 버퍼도, 디스크립터도, 동기화도 필요 없다.
    //
    // aspect를 여기서 계산하는 이유: extent는 리사이즈마다 바뀌는데 파이프라인은
    // 그대로다. 값이 커맨드에 실리니 파이프라인을 다시 만들 이유가 없다 -
    // 뷰포트를 동적 상태로 둔 것과 같은 이야기다.
    const PushConstants push{
        static_cast<float>(glfwGetTime()),
        static_cast<float>(extent.width) / static_cast<float>(extent.height),
    };
    vk.vkCmdPushConstants(cmd, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT,
                          0, sizeof(push), &push);

    // 정점 버퍼를 0번 슬롯에 건다. 파이프라인의 binding=0과 짝이다.
    // offset은 버퍼 안에서 시작할 바이트 - 여러 메시를 한 버퍼에 담으면 여기가 달라진다.
    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.handle, &offset);

    // 정점 6개 = 삼각형 2개, 인스턴스 1개. 한 드로우콜로 둘 다 나간다.
    vk.vkCmdDraw(cmd, 6, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

}

// ---- 패스 2: 패스 1의 결과를 읽어 스왑체인에 그린다 ----
//
// **draw를 받는 것이 "앞 패스의 결과를 읽는다"는 뜻이다.** 여기에 쓰지 않고 읽기만 한다:
//   draw.color   전이시켜서(SHADER_READ_ONLY) 샘플링한다
//   draw.colorSet 그 이미지를 가리키는 셋 - 같은 draw에서 나오니 짝이 어긋날 수 없다
static void RecordPresentPass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                              const RenderTargets& draw,
                              const SwapchainImage& present,
                              VkExtent2D presentExtent,
                              const Pipeline& fullscreen) noexcept {
    // ========================================================================
    // 여기까지가 첫 패스다. **스왑체인이 한 번도 안 나왔다.**
    // ========================================================================

    // ---- 첫 패스의 결과를 셰이더가 읽을 수 있게 ----
    // 그리기가 끝나야(COLOR_ATTACHMENT_OUTPUT) 읽을 수 있다(FRAGMENT_SHADER).
    // 레이아웃은 디스크립터를 채울 때 적어둔 SHADER_READ_ONLY_OPTIMAL과 같아야 한다.
    RecordLayoutTransition(vk, cmd, draw.color.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ---- 스왑체인 이미지를 그릴 수 있는 레이아웃으로 ----
    // **다시 색 첨부다** (블릿을 쓰던 동안은 TRANSFER_DST였다).
    // srcStage가 SubmitFrame의 wait.stageMask와 겹쳐야 한다 - 안 겹치면 이 전이가
    // acquire를 앞질러 실행될 수 있다 (동기화 검증이 잡아준 자리).
    RecordLayoutTransition(vk, cmd, present.image, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // ======== 두 번째 패스: 스왑체인에 전체화면 ========
    //
    // 첫 패스와 다른 것: **첨부가 스왑체인이고, 뎁스가 없고, 정점 버퍼가 없다.**
    // 크기도 다르다 - 여기는 창 크기(presentExtent)로 그린다. 렌더 해상도와 창 크기가
    // 다르면 샘플러의 LINEAR가 늘리거나 줄인다 (블릿이 하던 일이다).
    VkRenderingAttachmentInfo swapColor{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    swapColor.imageView = present.view;
    swapColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // 전체를 덮으니 지울 필요가 없다
    swapColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo presentPass{VK_STRUCTURE_TYPE_RENDERING_INFO};
    presentPass.renderArea.extent = presentExtent;
    presentPass.layerCount = 1;
    presentPass.colorAttachmentCount = 1;
    presentPass.pColorAttachments = &swapColor;

    vk.vkCmdBeginRendering(cmd, &presentPass);

    // **y를 안 뒤집는다.** 첫 패스는 GLM 규약을 맞추려고 뒤집었지만, 여기는 셰이더가
    // uv를 직접 만들어 쓰므로 뒤집으면 화면이 상하로 뒤집힌다.
    VkViewport presentViewport{};
    presentViewport.width = static_cast<float>(presentExtent.width);
    presentViewport.height = static_cast<float>(presentExtent.height);
    presentViewport.maxDepth = 1.0f;
    vk.vkCmdSetViewport(cmd, 0, 1, &presentViewport);

    VkRect2D presentScissor{};
    presentScissor.extent = presentExtent;
    vk.vkCmdSetScissor(cmd, 0, 1, &presentScissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fullscreen.handle);

    // **푸시 상수 대신 디스크립터 셋을 건다.** 값이 아니라 이미지라 커맨드에 실을 수 없고,
    // "이 셋을 0번 자리에 붙여라"만 명령으로 나간다.
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fullscreen.layout,
                               0, 1, &draw.colorSet, 0, nullptr);

    // 정점 3개, 정점 버퍼 없음. 셰이더가 gl_VertexIndex로 만든다.
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

    // ---- present 가능한 레이아웃으로 ----
    RecordLayoutTransition(vk, cmd, present.image, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

}

// 한 프레임의 기록. **패스 둘을 순서대로 부르는 것이 전부다.**
bool RecordFrame(const VolkDeviceTable& vk,
                 VkCommandBuffer cmd,
                 const FrameTarget& target,
                 const Pipeline& pipeline,
                 const Buffer& vertexBuffer,
                 const Pipeline& fullscreen) noexcept {
    // 풀을 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT로 만들었기에 버퍼 하나만
    // 되감을 수 있다. 그 플래그가 없으면 풀 전체를 리셋해야 한다.
    if (vk.vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
        LOG("[vk] vkResetCommandBuffer failed\n");
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    // ONE_TIME_SUBMIT: 한 번 제출하고 버릴 기록이라고 드라이버에 알린다.
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk.vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        LOG("[vk] vkBeginCommandBuffer failed\n");
        return false;
    }

    RecordScenePass(vk, cmd, *target.draw, pipeline, vertexBuffer);
    RecordPresentPass(vk, cmd, *target.draw, *target.present,
                      target.presentExtent, fullscreen);

    if (vk.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG("[vk] vkEndCommandBuffer failed\n");
        return false;
    }
    return true;
}

// ============================================================================
// main - 목차
// ============================================================================
int main() {
    // ========================================================================
    // 선언 - **파괴 역순으로 배치한다. 채우는 순서와 다르다.**
    // ========================================================================
    //
    // C++은 선언 순서의 역순으로 파괴한다. 그런데 우리의 **생성** 순서와 **파괴** 순서는
    // 같은 줄에 세울 수가 없다:
    //
    //   생성: 창/서피스가 디바이스보다 **먼저** (GPU 고를 때 서피스가 필요하다)
    //   파괴: 창의 스왑체인이 디바이스보다 **먼저** (디바이스가 만든 것이다)
    //
    // 둘 다 만족시키려면 **선언과 채우기를 떼어야 한다.** 전부 기본 생성 = 비어 있음이고
    // (그 상태가 합법이다) Create가 out 파라미터로 채우므로 가능하다.
    //
    // 여기 순서를 잘못 잡으면 검증 레이어가 잡아준다.
    WindowSystem   windowSystem;   // 파괴: 마지막. glfwTerminate는 모든 창 뒤에
    VulkanInstance inst;
    VulkanDevice   dev;
    Window         window;         // 스왑체인을 품는다 -> dev보다 먼저 죽어야 한다
    Commands       commands;
    Descriptors    descriptors;   // frames가 이 풀에서 셋을 받는다 -> frames보다 먼저 선언
    Frame          frames[kFramesInFlight];   // **한 벌씩. 배열이 된 게 전부다**
    Pipeline       pipeline;
    Pipeline       fullscreen;
    Buffer         vertexBuffer;   // 파괴: 첫 번째

    // ========================================================================
    // 채우기 - **의존 순서로.** 조기 return이 아무것도 안 샌다.
    // ========================================================================

    // ---- 플랫폼과 창 ----
    // glfwInit이 먼저인 것은 의존 때문이 아니다. windowSystem이 맨 먼저 선언돼 있어서
    // (= 가장 늦게 죽는다) 생성 순서를 거기 맞췄다.
    if (!InitWindowSystem(&windowSystem)) { return 1; }
    if (!CreateInstance(&inst)) { return 1; }
    if (!OpenWindow(inst, 1280, 720, "Lambda Engine", &window)) { return 1; }

    // ---- GPU를 고르고, GPU에게 물어볼 것을 다 묻는다 ----
    //
    // 포맷 둘 다 논리 디바이스가 필요 없다 (이유는 각 함수의 헤더에).
    // 나란히 둔 이유는 **포맷 계약이 둘**이기 때문이다:
    //   formats                우리 렌더 타겟이 쓸 것
    //   window.surfaceFormat   스왑체인이 쓸 것 - 두 번째 패스가 여기 맞춘다
    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    const RenderTargetFormats formats = ChooseRenderTargetFormats(inst, selection.gpu);
    if (formats.depth == VK_FORMAT_UNDEFINED) {
        LOG("[vk] no usable depth format\n");
        return 1;
    }
    if (!SelectSurfaceFormat(inst, selection.gpu, &window)) { return 1; }

    // ---- 디바이스와 거기 딸린 것들 ----
    // selection은 여기서 dev 안으로 흡수된다.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }
    if (!CreateCommands(dev, &commands)) { return 1; }
    if (!CreateDescriptors(dev, kFramesInFlight, &descriptors)) { return 1; }

    for (Frame& f : frames) {
        if (!CreateFrame(dev, commands, descriptors, formats, &f)) { return 1; }
    }

    // ---- 파이프라인 둘 ----
    // 계약의 상대가 다르다: 장면은 우리 렌더 타겟에, 전체화면은 스왑체인에 그린다.
    if (!CreateTrianglePipeline(dev, formats, &pipeline)) { return 1; }
    if (!CreateFullscreenPipeline(dev, window.surfaceFormat.format,
                                  descriptors.setLayout, &fullscreen)) {
        return 1;
    }

    // ---- 그릴 것 ----
    //
    // **삼각형 둘을 겹치게 두고 그리는 순서를 깊이 순서와 반대로 만들었다.**
    // 뎁스 테스트가 실제로 도는지 보는 방법이다 - 겹친 곳이 초록이면 켜진 것이고,
    // 빨강이면(나중에 그린 쪽) 꺼진 것이다. 삼각형 하나로는 이 차이가 안 보인다.
    constexpr Vertex kTriangles[] = {
        // 가까움 (z=0.25), 먼저 그린다 - 초록
        {{-0.7f,  0.5f, 0.25f}, {0.1f, 0.9f, 0.2f}},
        {{-0.7f, -0.5f, 0.25f}, {0.1f, 0.9f, 0.2f}},
        {{ 0.3f,  0.0f, 0.25f}, {0.1f, 0.9f, 0.2f}},

        // 멈 (z=0.75), 나중에 그린다 - 빨강
        {{ 0.7f,  0.5f, 0.75f}, {0.9f, 0.2f, 0.1f}},
        {{-0.3f,  0.0f, 0.75f}, {0.9f, 0.2f, 0.1f}},
        {{ 0.7f, -0.5f, 0.75f}, {0.9f, 0.2f, 0.1f}},
    };
    if (!CreateVertexBuffer(dev, commands, kTriangles, sizeof(kTriangles), &vertexBuffer)) {
        return 1;
    }

    // 스왑체인은 여기서 안 만든다. 루프의 EnsureSwapchain이 만들고, 최초 생성도
    // 재생성과 같은 경로다 - "지금 그릴 곳이 없다"가 시작 시점에도 정상이라서다.

    // ---- 루프 ----
    //
    // 동기화(그릴 곳 확보 · 대기 · acquire · 제출 · present)는 전부 Frame.cpp 안이다.
    // 여기 남은 것은 **프레임 하나의 모양**과, 실패했을 때 무엇을 할지뿐이다.
    LOG("close the window to exit.\n");

    // 어느 프레임 자원 한 벌을 쓸 차례인가. 매 프레임 돌아간다.
    uint32_t frameIndex = 0;

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // **최소화 중이면 이벤트가 올 때까지 잔다.** 이게 없으면 스왑체인을 못 만드는
        // 상태에서 매 순회 재생성을 시도하고, present가 없어 수직동기 제동도 없다.
        // 실측 CPU 10.9% -> 135.9%였다.
        if (!WindowHasDrawableSize(window)) {
            glfwWaitEvents();
            continue;
        }

        const Frame& frame = frames[frameIndex];

        FrameTarget target;
        const FrameResult begun = BeginFrame(dev, &window, frame, &target);
        if (begun == FrameResult::Fatal) { break; }
        if (begun == FrameResult::Skip) { continue; }

        // 아래 셋은 전부 continue가 아니라 **break**다. acquire까지 갔는데 제출을
        // 안 하면 신호된 세마포어와 리셋된 펜스를 기다릴 사람이 없어진다.
        if (!RecordFrame(dev.table, frame.cmd, target, pipeline, vertexBuffer,
                         fullscreen)) {
            break;
        }

        // 제출과 present가 갈라진 근거는 Frame.h에. **창이 없으면 아래 둘째 줄만 빠진다.**
        if (!SubmitFrame(dev, frame, target.present->renderFinished)) {
            break;
        }
        if (!PresentFrame(dev, &window, *target.present)) {
            break;
        }

        frameIndex = (frameIndex + 1) % kFramesInFlight;
    }

    // ---- 정리 ----
    //
    // **한 줄뿐이다.** 나머지는 전부 소멸자가 선언의 역순으로 한다:
    //   vertexBuffer -> pipeline -> frame -> commands -> window(스왑체인->서피스->창)
    //   -> dev -> inst -> windowSystem
    //
    // 한때 여기 열 줄이 있었고, 순서를 틀리면 조용히 깨졌다. 자원을 추가할 때마다
    // 한 줄 더 적어야 했고 잊으면 샜다. 지금은 필드를 추가하면 정리가 따라온다.
    //
    // GPU 대기는 남는다 - ~VulkanDevice가 vkDeviceWaitIdle을 부르지만, 그건
    // **다른 소멸자들이 다 돈 뒤**다. 스왑체인/커맨드 풀처럼 GPU가 아직 쓰고 있을 수
    // 있는 것들을 파괴하기 전에 한 번 기다려야 한다.
    // (각 소멸자가 자기 것을 기다리게 할 수도 있지만, 여기서 한 번이 더 싸고 명확하다.)
    dev.table.vkDeviceWaitIdle(dev.handle);

    LOG("[vk] clean shutdown\n");
    return 0;
}
