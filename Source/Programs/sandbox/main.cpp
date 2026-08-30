// **한 프레임이 어떻게 도는가** - 런타임 경로.
//
// 만들고 부수는 것은 전부 Vulkan/ 아래에 있다. 여기는 매 프레임 도는 코드만 있다.
//
// Vulkan/ 은 **개념당 한 쌍**이다 (언리얼 VulkanRHI와 같은 축):
//   Core.h       두 함수 테이블 · 요구사항 · RAII 규약
//   Instance     Window      Swapchain    Commands
//   Device       Frame       Pipeline     Buffer
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
#include "Vulkan/Frame.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Window.h"

#include <GLFW/glfw3.h>

// 8. 한 프레임 기록하기
// ============================================================================

// 프레임의 6~11단계: 배리어 -> 렌더링 시작 -> **드로우** -> 렌더링 끝 -> 배리어 -> 기록 끝.
//
// **동기화(펜스·세마포어·acquire·present)가 하나도 안 들어온다** - 그건 전부
// BeginFrame/EndFrame에 있다. 기록과 동기화는 서로 모르는 채로 돌아간다.
// 이게 "기록하는 쪽"과 "제출하는 쪽"이 갈리는 선이다.
//
// **인자가 오히려 줄었다** (6 -> 5). 스왑체인 이미지와 extent를 따로 받던 것이
// FrameTarget 하나로 합쳐졌다 - 그릴 곳과 내보낼 곳이 갈리면서 오히려 한 덩어리로
// 다룰 이유가 생겼다.
//
// **bool인 이유**: vkBegin/EndCommandBuffer는 실패할 수 있고(메모리 부족), 실패하면
// 커맨드 버퍼가 무효 상태다. 그걸 제출하는 것은 스펙 위반이라 호출자가 알아야 한다.
bool RecordFrame(const VolkDeviceTable& vk,
                 VkCommandBuffer cmd,
                 const FrameTarget& target,
                 const Pipeline& pipeline,
                 const Buffer& vertexBuffer) noexcept {
    const RenderTargets& draw = *target.draw;
    const VkExtent2D extent = draw.extent;   // **창 크기가 아니다.** Config.h가 정한다
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

    // ========================================================================
    // 여기까지가 렌더링이다. **스왑체인이 한 번도 안 나왔다.**
    // 아래는 결과를 화면으로 내보내는 일이고, 창이 없으면 통째로 없어도 되는 부분이다.
    // ========================================================================

    // ---- 우리 색 이미지를 전송원으로 ----
    // 그리기가 끝나야(COLOR_ATTACHMENT_OUTPUT) 읽을 수 있다(TRANSFER + TRANSFER_READ).
    RecordLayoutTransition(vk, cmd, draw.color.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // ---- 스왑체인 이미지를 전송지로 ----
    // UNDEFINED에서 시작하는 이유는 색 첨부 때와 같다 - 블릿이 전부 덮어쓴다.
    RecordLayoutTransition(vk, cmd, target.present->image, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // ---- 블릿 ----
    //
    // **복사(vkCmdCopyImage)가 아니라 블릿인 이유**: 크기와 포맷이 다를 수 있다.
    // 복사는 둘 다 같아야 하는데, 렌더 해상도는 Config.h가 정하고 창 크기는 사용자가
    // 정한다. 블릿은 필터링하며 늘리거나 줄여준다 - 창을 리사이즈해도 렌더 해상도가
    // 그대로인 것이 여기서 흡수된다.
    //
    // (전제: 두 포맷이 BLIT_SRC / BLIT_DST를 지원해야 한다. 8비트 RGBA류는 사실상
    //  모든 구현이 지원하고, 아니면 검증 레이어가 크게 알려준다.)
    VkImageBlit region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.srcOffsets[1] = VkOffset3D{static_cast<int32_t>(extent.width),
                                      static_cast<int32_t>(extent.height), 1};
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.dstOffsets[1] = VkOffset3D{static_cast<int32_t>(target.presentExtent.width),
                                      static_cast<int32_t>(target.presentExtent.height), 1};

    vk.vkCmdBlitImage(cmd,
                      draw.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      target.present->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &region, VK_FILTER_LINEAR);

    // ---- present 가능한 레이아웃으로 ----
    // **뎁스는 아무 전이도 안 한다.** 화면에 나갈 일이 없고 다음 프레임에 다시
    // UNDEFINED에서 시작한다.
    RecordLayoutTransition(vk, cmd, target.present->image, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

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
    Frame          frames[kFramesInFlight];   // **한 벌씩. 배열이 된 게 전부다**
    Pipeline       pipeline;
    Buffer         vertexBuffer;   // 파괴: 첫 번째

    // ========================================================================
    // 채우기 - **의존 순서로.**
    // ========================================================================
    //
    // 조기 return이 아무것도 안 샌다. 여기까지 채워진 것은 소멸자가 알아서 정리한다.
    if (!CreateInstance(&inst)) { return 1; }
    if (!InitWindowSystem(&windowSystem)) { return 1; }
    if (!OpenWindow(inst, 1280, 720, "Lambda Engine", &window)) { return 1; }

    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    // selection은 여기서 dev 안으로 흡수되고 더 이상 쓰이지 않는다.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }

    // 이 창이 받는 포맷을 확정한다. **GPU가 정해진 뒤에만 알 수 있다** -
    // 어떤 포맷을 받는지는 (GPU, 서피스) 쌍이 정한다. 리사이즈로는 안 바뀐다.
    if (!SelectSurfaceFormat(inst, dev, &window)) { return 1; }

    // 큐 패밀리마다 풀 하나. 디바이스 수명이다.
    if (!CreateCommands(dev, &commands)) { return 1; }

    // frames-in-flight마다 한 벌.
    for (Frame& f : frames) {
        if (!CreateFrame(dev, commands, &f)) { return 1; }
    }

    // 파이프라인은 **포맷**에 묶인다 (크기는 동적 상태라 안 묶인다).
    // **창 포맷이 아니라 우리 렌더 타겟 포맷이다.** 파이프라인이 그리는 곳은
    // 오프스크린 이미지고, 스왑체인 포맷과는 블릿이 매개한다.
    if (!CreateTrianglePipeline(dev, kRenderColorFormat, &pipeline)) { return 1; }

    // 정점 데이터. y-up 규약이다.
    //
    // **삼각형 둘을 겹치게 두고, 그리는 순서를 깊이 순서와 반대로 만들었다.**
    // 이게 뎁스 테스트가 실제로 도는지 보는 방법이다:
    //
    //   뎁스 켜짐 -> 겹친 곳이 **초록**(가까운 쪽). 나중에 그린 빨강이 밀려난다
    //   뎁스 꺼짐 -> 겹친 곳이 **빨강**(나중에 그린 쪽). 덮어쓰기만 일어난다
    //
    // 삼각형 하나로는 이 차이가 안 보인다. 전에 z를 전부 0으로 두고도 잘 그려졌던
    // 이유이기도 하다.
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

    // 스왑체인은 루프의 EnsureSwapchain이 만든다 - 최초 생성도 재생성과 같은 경로다.
    // "지금 그릴 곳이 없다"가 시작 시점에도 정상 상태라(최소화된 채로 실행 가능)
    // 특별 취급이 필요 없다.

    // ---- 루프 ----
    //
    // **여덟 줄이다.** 동기화(그릴 곳 확보 · 대기 · acquire · 제출 · present)는 전부
    // BeginFrame/EndFrame 안으로 갔다. 여기 남은 것은 프레임의 **모양**뿐이다.
    //
    // 둘을 가른 근거: **바뀌는 이유가 다르다.**
    //   드로우·텍스처·디스크립터를 추가하면  -> RecordFrame만 바뀐다
    //   frames-in-flight·present 모드를 바꾸면 -> Frame.cpp만 바뀐다
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

        // 기록이 실패하면 **제출하지 않고 끝낸다.** 무효한 커맨드 버퍼를 제출하는 것은
        // 스펙 위반이고, 여기서 continue하면 이미 신호된 imageAvailable을 기다릴 사람이
        // 없어진 채로 다음 acquire가 같은 세마포어를 다시 신호하게 된다.
        if (!RecordFrame(dev.table, frame.cmd, target, pipeline, vertexBuffer)) {
            break;
        }

        // **여기는 continue가 아니라 break다.** 제출이 실패하면 이 프레임의 펜스를
        // 신호할 사람이 없어 다음 순회가 영원히 걸린다. present의 회복 불가 에러도
        // 여기로 온다 (Frame.cpp 참고).
        if (!EndFrame(dev, &window, frame, target)) {
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
