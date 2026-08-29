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
    // ---- 만든다 (위 함수들 순서대로) ----
    //
    // out 파라미터가 하나도 없다. 묶음이 곧 반환값이다.
    const VulkanInstance inst = CreateInstance();
    if (inst.handle == VK_NULL_HANDLE) { return 1; }

    if (!InitWindowSystem()) { return 1; }

    // 창 + 서피스. 스왑체인은 디바이스가 생긴 뒤라 아직 비어 있다(= 최소화와 같은 정상 상태).
    Window window;
    if (!OpenWindow(inst, 1280, 720, "Lambda Engine", &window)) { return 1; }

    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    // selection은 여기서 dev 안으로 흡수되고 더 이상 쓰이지 않는다.
    const VulkanDevice dev = CreateDevice(inst, selection);
    if (dev.handle == VK_NULL_HANDLE) { return 1; }

    // 이 창이 받는 포맷을 확정한다. **GPU가 정해진 뒤에만 알 수 있다** -
    // 어떤 포맷을 받는지는 (GPU, 서피스) 쌍이 정한다. 리사이즈로는 안 바뀐다.
    if (!SelectSurfaceFormat(inst, dev, &window)) { return 1; }

    // 스왑체인은 루프의 EnsureSwapchain이 만든다 - 최초 생성도 재생성과 같은 경로다.
    // "지금 그릴 곳이 없다"가 시작 시점에도 정상 상태라(최소화된 채로 실행 가능)
    // 특별 취급이 필요 없다.

    // 큐 패밀리마다 풀 하나. 디바이스 수명이다.
    Commands commands;
    if (!CreateCommands(dev, &commands)) { return 1; }

    // frames-in-flight마다 한 벌. 지금은 1이라 하나다.
    Frame frame;
    if (!CreateFrame(dev, commands, &frame)) { return 1; }

    // 파이프라인은 **포맷**에 묶인다 (크기는 동적 상태라 안 묶인다).
    // 포맷이 창에 있으므로 스왑체인을 기다릴 필요가 없다 - 한때 여기서 포맷 하나를
    // 얻으려고 EnsureSwapchain을 미리 부르고, 루프 첫 바퀴가 또 불렀다.
    Pipeline pipeline = CreateTrianglePipeline(dev, window.surfaceFormat.format);
    if (pipeline.handle == VK_NULL_HANDLE) { return 1; }

    // 정점 데이터. y-up 규약이고 감는 방향은 CCW(파이프라인 frontFace와 일치).
    constexpr Vertex kTriangle[] = {
        {{ 0.0f,  0.5f}, {1.0f, 0.0f, 0.0f}},   // 위      - 빨강
        {{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},   // 왼쪽아래 - 초록
        {{ 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},   // 오른쪽아래 - 파랑
    };
    Buffer vertexBuffer = CreateVertexBuffer(dev, commands, kTriangle, sizeof(kTriangle));
    if (vertexBuffer.handle == VK_NULL_HANDLE) { return 1; }

    // ---- 루프 ----
    LOG("close the window to exit.\n");

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // 0. 그릴 곳 확보. 리사이즈 통보는 창 콜백이 window에 직접 세워놨다.
        if (!EnsureSwapchain(inst, dev, &window)) {
            continue;   // 최소화 중. 이번 프레임은 없다
        }
        Swapchain& swapchain = window.swapchain;

        // 1. 이전 프레임이 끝나기를 기다린다 (GPU -> CPU)
        dev.table.vkWaitForFences(dev.handle, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

        // 2. 이미지를 하나 빌린다
        uint32_t imageIndex = 0;
        const VkResult acquired = dev.table.vkAcquireNextImageKHR(
            dev.handle, swapchain.handle, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);

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
    }

    // ---- 정리: 만든 역순 ----
    //
    // **여기가 통째로 사라지는 것이 클래스로 옮기는 진짜 이유다.** RAII로 가면
    // 이 열 줄이 "선언 순서"로 표현되고, 순서를 틀릴 방법 자체가 없어진다.
    dev.table.vkDeviceWaitIdle(dev.handle);   // GPU가 아직 작업 중일 수 있다

    DestroyBuffer(dev, &vertexBuffer);
    DestroyPipeline(dev, &pipeline);
    DestroyFrame(dev, &frame);
    DestroyCommands(dev, &commands);   // 커맨드 버퍼도 풀과 함께 사라진다

    // 창에 묶인 셋(스왑체인 -> 서피스 -> 창)을 중첩 역순으로. **디바이스보다 먼저다** -
    // 스왑체인이 디바이스로 만들어졌기 때문이다.
    CloseWindow(inst, dev, &window);

    dev.table.vkDestroyDevice(dev.handle, nullptr);
    ShutdownWindowSystem();

    if (inst.messenger != VK_NULL_HANDLE) {
        inst.table.vkDestroyDebugUtilsMessengerEXT(inst.handle, inst.messenger, nullptr);
    }
    inst.table.vkDestroyInstance(inst.handle, nullptr);

    LOG("[vk] clean shutdown\n");
    return 0;
}
