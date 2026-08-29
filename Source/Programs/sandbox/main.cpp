// **한 프레임이 어떻게 도는가** - 런타임 경로.
//
// 만들고 부수는 것은 전부 VulkanSetup.cpp에 있다. 여기는 매 프레임 도는 코드만 있다.
// 무엇이 있는지는 Vulkan.h.
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

#include "Vulkan.h"

#include <GLFW/glfw3.h>

#include <chrono>

// 8. 한 프레임 기록하기
// ============================================================================

// 이미지 레이아웃 전이. 프레임에 두 번 나오는데 방향만 다르다.
//
// GPU 이미지는 **용도마다 내부 배치가 다르다.** "렌더 타겟으로 쓸 때 빠른 배치"와
// "화면에 내보낼 때의 배치"가 다르고, 그 사이를 명시적으로 바꿔줘야 한다.
// 배리어는 그 전환과 함께 **메모리 가시성**(앞의 쓰기가 뒤의 읽기에 보이는가)도 처리한다.
void RecordLayoutTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd, VkImage image,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                            VkImageLayout oldLayout, VkImageLayout newLayout) noexcept {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vk.vkCmdPipelineBarrier2(cmd, &dep);
}

// 프레임의 6~11단계: 배리어 -> 렌더링 시작 -> **드로우** -> 렌더링 끝 -> 배리어 -> 기록 끝.
//
// **인자가 넷뿐인 것에 주목.** 동기화(펜스·세마포어·acquire·present)가 하나도 안 들어온다 -
// 그건 전부 main()의 루프에 남아 있다. 기록과 동기화는 서로 모르는 채로 돌아간다.
// 이게 나중에 "기록하는 쪽"과 "제출하는 쪽"이 갈리는 선이다.
void RecordFrame(const VolkDeviceTable& vk,
                 VkCommandBuffer cmd,
                 const SwapchainImage& target,
                 VkExtent2D extent,
                 const Pipeline& pipeline,
                 const Buffer& vertexBuffer) noexcept {
    vk.vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    // ONE_TIME_SUBMIT: 한 번 제출하고 버릴 기록이라고 드라이버에 알린다.
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(cmd, &beginInfo);

    // ---- 그릴 수 있는 레이아웃으로 ----
    // oldLayout이 UNDEFINED인 것은 이전 내용을 안 쓰기 때문이다 - 어차피 loadOp=CLEAR로
    // 덮는다. 보존을 요구하면 드라이버가 실제로 복사를 해야 한다.
    RecordLayoutTransition(vk, cmd, target.image,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // ---- 렌더링 시작 ----
    // 다이나믹 렌더링: VkRenderPass/VkFramebuffer 객체를 미리 만들지 않는다.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

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

    // 정점 버퍼를 0번 슬롯에 건다. 파이프라인의 binding=0과 짝이다.
    // offset은 버퍼 안에서 시작할 바이트 - 여러 메시를 한 버퍼에 담으면 여기가 달라진다.
    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.handle, &offset);

    // 정점 3개, 인스턴스 1개.
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

    // ---- present 가능한 레이아웃으로 ----
    RecordLayoutTransition(vk, cmd, target.image,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    vk.vkEndCommandBuffer(cmd);
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
    // 포맷이 창에 있으므로 스왑체인을 기다릴 필요가 없다.
    if (!CreateTrianglePipeline(dev, window.surfaceFormat.format, &pipeline)) { return 1; }

    // 정점 데이터. y-up 규약이고 감는 방향은 CCW(파이프라인 frontFace와 일치).
    constexpr Vertex kTriangle[] = {
        {{ 0.0f,  0.5f}, {1.0f, 0.0f, 0.0f}},   // 위        - 빨강
        {{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},   // 왼쪽아래   - 초록
        {{ 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},   // 오른쪽아래 - 파랑
    };
    if (!CreateVertexBuffer(dev, commands, kTriangle, sizeof(kTriangle), &vertexBuffer)) {
        return 1;
    }

    // 스왑체인은 루프의 EnsureSwapchain이 만든다 - 최초 생성도 재생성과 같은 경로다.
    // "지금 그릴 곳이 없다"가 시작 시점에도 정상 상태라(최소화된 채로 실행 가능)
    // 특별 취급이 필요 없다.

    // ---- 루프 ----
    LOG("close the window to exit.\n");

    // 어느 프레임 자원 한 벌을 쓸 차례인가. 매 프레임 돌아간다.
    uint32_t frameIndex = 0;

    // [임시 계측] "frames-in-flight를 늘리면 뭐가 좋아지나"를 재기 위한 것.
    // 런타임 경로에 로그가 들어가지만 초당 한 번으로 묶어서 폭주하지 않는다.
    uint64_t frameCount = 0;
    double fenceSeconds = 0.0;
    double acquireSeconds = 0.0;
    auto reportAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // 0. 그릴 곳 확보. 리사이즈 통보는 창 콜백이 window에 직접 세워놨다.
        if (!EnsureSwapchain(inst, dev, &window)) {
            continue;   // 최소화 중. 이번 프레임은 없다
        }
        Swapchain& swapchain = *window.swapchain;

        Frame& frame = frames[frameIndex];

        // 1. 이전 프레임이 끝나기를 기다린다 (GPU -> CPU)
        //
        // **여기가 frames-in-flight의 값어치가 드러나는 자리다.** 1이면 바로 직전
        // 프레임을 기다리고, 2면 두 프레임 전 것을 기다린다 - 그동안 GPU가 앞선다.
        const auto waitBegin = std::chrono::steady_clock::now();
        dev.table.vkWaitForFences(dev.handle, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
        fenceSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - waitBegin).count();

        // 2. 이미지를 하나 빌린다
        uint32_t imageIndex = 0;
        const auto acquireBegin = std::chrono::steady_clock::now();
        const VkResult acquired = dev.table.vkAcquireNextImageKHR(
            dev.handle, swapchain.handle, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        acquireSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - acquireBegin).count();

        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            window.swapchainOutOfDate = true;
            continue;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            LOG("[vk] vkAcquireNextImageKHR failed (%d)\n", acquired);
            break;
        }

        const SwapchainImage& target = swapchain.images[imageIndex];

        // 3. 펜스 리셋 (**acquire가 성공한 뒤에**)
        // 먼저 리셋하면, acquire가 실패해 제출 없이 돌아가는 프레임에서 펜스가 영영
        // 신호되지 않고 다음 WaitForFences가 영원히 걸린다.
        dev.table.vkResetFences(dev.handle, 1, &frame.inFlight);

        // 4~11. 기록
        RecordFrame(dev.table, frame.cmd, target, swapchain.extent, pipeline, vertexBuffer);

        // 12. 제출
        // acquire가 끝나야 이미지에 쓸 수 있고(wait), 다 쓰면 present가 알아야 한다(signal).
        // 기다리는 지점을 COLOR_ATTACHMENT_OUTPUT으로 좁히면 그 앞 스테이지는 미리 돈다.
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = frame.imageAvailable;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = target.renderFinished;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmdInfo.commandBuffer = frame.cmd;

        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;

        if (dev.table.vkQueueSubmit2(dev.queues.graphics, 1, &submit, frame.inFlight) != VK_SUCCESS) {
            LOG("[vk] vkQueueSubmit2 failed\n");
            break;
        }

        // 13. 화면에 내보낸다
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &target.renderFinished;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain.handle;
        present.pImageIndices = &imageIndex;

        // SUBOPTIMAL은 에러가 아니다. 그려지긴 했고 다음 프레임에 다시 만들면 된다.
        const VkResult presented = dev.table.vkQueuePresentKHR(dev.queues.present, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            window.swapchainOutOfDate = true;
        }

        // 다음 차례. **이 한 줄이 frames-in-flight를 돌리는 전부다.**
        frameIndex = (frameIndex + 1) % kFramesInFlight;

        // [임시 계측] 초당 한 번
        ++frameCount;
        const auto now = std::chrono::steady_clock::now();
        if (now >= reportAt) {
            LOG("[perf] in-flight=%u | %llu fps | fence %.1f%% | acquire %.1f%%\n",
                kFramesInFlight, static_cast<unsigned long long>(frameCount),
                fenceSeconds * 100.0, acquireSeconds * 100.0);
            frameCount = 0;
            fenceSeconds = 0.0;
            acquireSeconds = 0.0;
            reportAt = now + std::chrono::seconds(1);
        }
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
