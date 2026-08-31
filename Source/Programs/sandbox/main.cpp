// 흐름이 있는 곳 - 무엇을 어떤 순서로 만들고, 한 frame이 어떻게 도는가.
//
// 각 자원을 어떻게 만들고 부수는지는 Vulkan/ 아래에 있다. 여기는 순서만 있다.
// Vulkan/ 은 개념당 .h/.cpp 한 쌍이라 자원이 늘어도 기존 파일이 안 자란다.
//
// 초기화와 런타임은 지켜야 할 규칙이 다르고, 그게 파일을 나눈 기준이다:
//
//               초기화     런타임 (매 frame)
//   실행 횟수   1회        초당 수백
//   heap 할당   마음껏     금지
//   log         자유롭게   지속 조건이면 폭주한다
//   실패 처리   되돌린다   frame을 버리거나 복구


#include "Config.h"
#include "Vulkan/Barrier.h"
#include "Vulkan/Buffer.h"
#include "Vulkan/Commands.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Window.h"

#include <GLFW/glfw3.h>

// 필요한 것만 하나씩 include한다. <glm/ext.hpp>는 벤더링할 때 뺐다 (VERSION.md).
#include <glm/ext/matrix_clip_space.hpp>   // perspective
#include <glm/ext/matrix_transform.hpp>    // rotate · lookAt

// Frame 기록
// ============================================================================
//
// 여기 나오는 것들의 관계 - 셋으로 갈린다.
//
//   서로 맞아야 하는 것       format · pipeline · vertex buffer
//   명령이 적히는 곳          command buffer
//   동시에 여러 개 돌리는 것  frame
//
// Command buffer와 frame은 "무엇을 그리나"에 아무 말도 안 한다. 앞은 명령을 적는
// 테이프고, 뒤는 그 한 벌을 kFramesInFlight개 둬서 CPU가 앞서가게 하는 장치다.
//
// 맞아야 하는 것은 둘이고 pipeline이 그 가운데에 있다:
//   pipeline <-> render target   format (dynamic rendering이 pipeline에 박는다)
//   pipeline <-> vertex buffer   vertex layout (attribute description <-> Vertex)
//
// Pass는 vkCmdBeginRendering ~ vkCmdEndRendering 구간이고 그동안 그릴 대상이
// 고정된다. 한 frame에 pass가 둘이고 뒤가 앞의 결과를 읽는다:
//
//   [Scene Pass]  창을 모른다
//     barrier x2 (우리 color·depth)
//     +- BeginRendering --- attachment = draw.color / draw.depth
//     |    BindPipeline        format이 attachment와 같아야 한다
//     |    BindVertexBuffers   layout이 pipeline과 같아야 한다
//     |    Draw x2             면으로
//     |    BindPipeline        선으로 바꾼다 - 상태 전환이다
//     |    Draw
//     +- EndRendering
//
//   [Present Pass]  draw를 읽기만 한다
//     barrier (draw.color -> SHADER_READ_ONLY)
//     barrier (swapchain  -> COLOR_ATTACHMENT)
//     +- BeginRendering --- attachment = swapchain image
//     |    BindPipeline        이쪽 format은 swapchain 것이다
//     |    BindDescriptorSets  draw.colorSet이 draw.color를 가리킨다
//     |    Draw                vertex buffer 없음
//     +- EndRendering
//     barrier (swapchain -> PRESENT_SRC)
//
// 동기화(fence · semaphore · acquire · present)는 하나도 안 들어온다. 그건 Frame.cpp에
// 있고, 기록과 동기화는 서로 모르는 채로 돌아간다.

// Scene Pass
//
// Input:  cmd, draw, pipeline 둘, vertex buffer
// Effect: draw.color / draw.depth에 그리는 명령이 cmd에 append된다
//
// Swapchain이 인자에 없다 - 창이 없어도 성립한다.
//
// pipeline이 둘이 되면서 **이 층에 처음으로 고를 것이 생겼다.** 어느 물체를 어느
// pipeline으로 그릴지는 여기서 정한다 - 위에서는 둘을 만들어 넘기기만 한다.
static void RecordScenePass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                            const RenderTargets& draw,
                            const Pipeline& pipeline,
                            const Pipeline& wireframe,
                            const Pipeline& translucent,
                            const Buffer& vertexBuffer) noexcept {
    const VkExtent2D extent = draw.extent;   // **창 크기가 아니다.** Config.h가 정한다
    // oldLayout이 UNDEFINED인 이유: 이전 내용을 안 쓴다(loadOp=CLEAR로 덮는다).
    // 보존을 요구하면 driver가 실제로 복사를 한다.
    RecordLayoutTransition(vk, cmd, draw.color.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Stage가 color와 다르다. Depth test는 EARLY/LATE_FRAGMENT_TESTS에서 일어나고
    // 이건 COLOR_ATTACHMENT_OUTPUT보다 앞이다. Color barrier의 stage를 그대로 쓰면
    // depth write가 barrier보다 먼저 일어날 수 있다.
    RecordLayoutTransition(vk, cmd, draw.depth.handle, VK_IMAGE_ASPECT_DEPTH_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                               | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = draw.color.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    // Clear 값 1.0 = 가장 멈. Pipeline의 compareOp=LESS와 짝이다 - 0.0으로 clear하면
    // 아무것도 통과 못 해 화면이 빈다.
    // storeOp가 DONT_CARE인 이유: depth는 이 frame 안에서만 쓰인다. 다음 frame에
    // 필요해지면(SSAO 같은 post-processing) STORE로 바꾼다.
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

    // Viewport/scissor를 dynamic state로 둔 덕에 창 크기가 바뀌어도 pipeline을 다시
    // 만들 필요가 없다.
    //
    // 부호를 여기서 안 정한다. pipeline이 든 값을 그대로 넘기므로 그쪽의 frontFace와
    // 어긋날 수가 없다 (Pipeline.h). 여기는 y-up이라 뒤집혀 나온다.
    const VkViewport viewport = MakeViewport(extent, pipeline.viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);

    // 시저: 이 사각형 밖의 픽셀은 버린다. 지금은 화면 전체다.
    VkRect2D scissor{};
    scissor.extent = extent;
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);

    const float t = static_cast<float>(glfwGetTime());

    // 카메라. 오른손 좌표계라 -z 쪽을 본다.
    //
    // z=2에 둔 이유는 화면에 차는 크기를 전과 비슷하게 맞추려는 것뿐이다.
    // fov 60도에서 거리 2면 보이는 반높이가 2*tan(30) = 1.155이고, 삼각형 반높이가
    // 0.5라 화면의 43%를 차지한다 (전에는 NDC에 직접 적어서 50%였다).
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 2.0f),    // eye
                                       glm::vec3(0.0f, 0.0f, 0.0f),    // center
                                       glm::vec3(0.0f, 1.0f, 0.0f));   // up

    // 여기 extent는 draw.extent라 Config.h가 정한 고정값이다 - 리사이즈로 안 바뀌고
    // 지금 aspect는 상수(1280/720)다. 그래도 매 frame 계산하는 이유는 값이 command에
    // 실리기 때문이다: render 해상도가 런타임 값이 되어도 pipeline은 그대로다.
    // 달라진 건 shader의 `x /= aspect` 한 줄이 하던 일을 이제 proj가 한다는 것이다.
    const float aspect =
        static_cast<float>(extent.width) / static_cast<float>(extent.height);

    // **proj[1][1]에 -1을 곱하지 않는다.** 흔히 보이는 그 줄은 viewport height가
    // 양수일 때 쓰는 것이고, 우리는 위에서 이미 음수로 줬다. 둘 다 하면 이중 반전이라
    // 화면이 상하로 뒤집힌다.
    //
    // 깊이가 [0,1]로 나오는 것은 GLM_FORCE_DEPTH_ZERO_TO_ONE 덕이고, 그건 CMake의
    // glm 타깃에 붙어 있어서 여기서 신경 쓸 것이 없다. 없으면 OpenGL 규약([-1,1])이
    // 되어 가까운 절반이 잘려 나가는데 아무도 경고해주지 않는다.
    //
    // near를 0.1로 잡았다. 깊이 정밀도는 near 근처에 몰리므로 near를 키울수록
    // 멀리서 z-fighting이 줄어든다 - 물체가 늘어나면 그때 만질 손잡이다.
    const glm::mat4 proj =
        glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);

    // binding 0은 pipeline의 binding=0과 짝이다. offset은 buffer 안의 시작 바이트 -
    // 여러 mesh를 한 buffer에 담으면 여기가 달라진다.
    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.handle, &offset);

    // 물체 셋을 일부러 접지 않고 평평하게 적는다. 무엇이 반복되고 무엇이 다른지를
    // 눈으로 보려는 것이다 - 접는 것은 그 다음이다.

    // 1. 초록. 원점에서 z축 회전 (축이 z라 깊이가 안 바뀐다)
    const glm::mat4 model0 =
        glm::rotate(glm::mat4(1.0f), t, glm::vec3(0.0f, 0.0f, 1.0f));
    const PushConstants push0{proj * view * model0, 1.0f};
    vk.vkCmdPushConstants(cmd, pipeline.layout,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(push0), &push0);
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    // 2. 빨강. 반대 방향으로 더 천천히
    const glm::mat4 model1 =
        glm::rotate(glm::mat4(1.0f), -t * 0.5f, glm::vec3(0.0f, 0.0f, 1.0f));
    const PushConstants push1{proj * view * model1, 1.0f};
    vk.vkCmdPushConstants(cmd, pipeline.layout,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(push1), &push1);
    vk.vkCmdDraw(cmd, 3, 1, 3, 0);

    // 3. 초록을 한 번 더. **정점을 안 늘리고 물체만 늘었다** - vertex 범위는 1번과
    //    같고 transform만 다르다. 그리고 이번엔 선으로 그린다.
    //
    // **bind가 draw 사이에서 일어난다.** 여기까지 오면 command buffer가 상태 기계라는
    // 것이 코드에 보인다 - 이 줄 위의 draw 둘은 면으로, 아래는 선으로 나간다.
    //
    // 오버레이(같은 물체를 면+선으로 두 번)가 아닌 이유: 깊이가 같아서 compareOp=LESS에
    // 선이 전부 걸린다. 그러려면 wireframe 쪽만 LESS_OR_EQUAL이어야 하고 그건 pipeline이
    // 두 값에서 갈린다는 뜻이다 - 지금은 한 값(polygonMode)만 갈린 것을 보려 한다.
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wireframe.handle);

    const glm::mat4 model2 =
        glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, -0.6f, -0.5f)),
                   glm::vec3(0.5f));
    const PushConstants push2{proj * view * model2, 1.0f};
    // layout이 pipeline.layout이 아니라 wireframe.layout이다. 둘은 정의가 같아서
    // 호환되지만(값이 살아남는다) 지금 bind된 것을 적는 편이 읽기에 정직하다.
    vk.vkCmdPushConstants(cmd, wireframe.layout,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(push2), &push2);
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    // 4. 반투명 파랑. **마지막에 그리는 것이 이 물체의 정확성 조건이다.**
    //
    // depth write를 껐으므로 이 물체는 자기 깊이를 안 남긴다. 그래서 뒤에 무엇을
    // 그리든 이것에 가려지지 않는다 - 순서를 지키는 일이 depth에서 기록 쪽으로
    // 넘어왔다. 지금은 반투명이 하나뿐이라 "맨 뒤"로 충분하지만, 둘이 되는 순간
    // 뒤에서 앞으로 정렬해야 한다.
    //
    // depth test는 살아 있다. z=0.5라 앞의 셋보다 카메라에 가까워서 다 통과한다.
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, translucent.handle);

    // 안 움직인다. Transform이 아무것도 아닐 수도 있다는 것이 여기서 보인다.
    const PushConstants push3{proj * view, 0.5f};
    vk.vkCmdPushConstants(cmd, translucent.layout,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(push3), &push3);
    vk.vkCmdDraw(cmd, 3, 1, 6, 0);

    vk.vkCmdEndRendering(cmd);

}

// Present Pass
//
// Input:  cmd, draw(읽기만), present, presentExtent, fullscreen
// Effect: draw.color를 sampling해 swapchain image에 그리는 명령이 cmd에 append된다
//
// draw를 받는 것이 "앞 pass의 결과를 읽는다"는 뜻이다. colorSet이 같은 draw에서
// 나오므로 짝이 어긋날 수 없다.
static void RecordPresentPass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                              const RenderTargets& draw,
                              const SwapchainImage& present,
                              VkExtent2D presentExtent,
                              const Pipeline& fullscreen) noexcept {
    // 그리기가 끝나야(COLOR_ATTACHMENT_OUTPUT) 읽을 수 있다(FRAGMENT_SHADER).
    // Layout은 descriptor를 채울 때 적어둔 SHADER_READ_ONLY_OPTIMAL과 같아야 한다.
    RecordLayoutTransition(vk, cmd, draw.color.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // srcStage가 SubmitFrame의 wait.stageMask와 겹쳐야 한다. 안 겹치면 이 transition이
    // acquire를 앞질러 실행될 수 있다 - sync validation이 잡아준 자리다.
    RecordLayoutTransition(vk, cmd, present.image, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Scene pass와 다른 것: attachment가 swapchain이고, depth가 없고, vertex buffer가
    // 없고, 크기가 창 크기(presentExtent)다. 렌더 해상도와 창 크기가 다르면 sampler의
    // LINEAR가 늘리거나 줄인다.
    VkRenderingAttachmentInfo swapColor{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    swapColor.imageView = present.view;
    swapColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // 전체를 덮으니 clear 불필요
    swapColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo presentPass{VK_STRUCTURE_TYPE_RENDERING_INFO};
    presentPass.renderArea.extent = presentExtent;
    presentPass.layerCount = 1;
    presentPass.colorAttachmentCount = 1;
    presentPass.pColorAttachments = &swapColor;

    vk.vkCmdBeginRendering(cmd, &presentPass);

    // Scene pass와 같은 자리에서 나오는데 부호가 반대다 - fullscreen pipeline이
    // ViewportY::Down으로 만들어졌기 때문이다. 그 이유는 그쪽에 적혀 있다.
    const VkViewport presentViewport = MakeViewport(presentExtent, fullscreen.viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &presentViewport);

    VkRect2D presentScissor{};
    presentScissor.extent = presentExtent;
    vk.vkCmdSetScissor(cmd, 0, 1, &presentScissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fullscreen.handle);

    // Push constant 대신 descriptor set. 값이 아니라 image라 command에 실을 수 없고
    // "이 set을 0번 자리에 붙여라"만 명령으로 나간다.
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fullscreen.layout,
                               0, 1, &draw.colorSet, 0, nullptr);

    // vertex 3개, buffer 없음. Shader가 gl_VertexIndex로 만든다.
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

    RecordLayoutTransition(vk, cmd, present.image, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

}

// Input:  cmd, target, pipeline, vertex buffer, fullscreen
// Effect: cmd를 리셋하고 pass 둘을 기록한다
// Output: false면 cmd가 무효 상태다 - 제출하면 안 된다
bool RecordFrame(const VolkDeviceTable& vk,
                 VkCommandBuffer cmd,
                 const FrameTarget& target,
                 const Pipeline& pipeline,
                 const Pipeline& wireframe,
                 const Pipeline& translucent,
                 const Buffer& vertexBuffer,
                 const Pipeline& fullscreen) noexcept {
    // Pool에 RESET_COMMAND_BUFFER_BIT을 줬기에 buffer 하나만 되감을 수 있다.
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

    RecordScenePass(vk, cmd, *target.draw, pipeline, wireframe, translucent,
                    vertexBuffer);
    RecordPresentPass(vk, cmd, *target.draw, *target.present,
                      target.presentExtent, fullscreen);

    if (vk.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG("[vk] vkEndCommandBuffer failed\n");
        return false;
    }
    return true;
}

int main() {
    // 선언 - 파괴의 역순. 아래 채우는 순서와 다르다.
    // ========================================================================
    //
    // C++은 선언의 역순으로 파괴하는데, 만드는 순서와 부수는 순서를 한 줄로 세울 수 없다:
    //   만들 때  창/surface가 device보다 먼저 (GPU를 고를 때 surface가 필요하다)
    //   부술 때  창의 swapchain이 device보다 먼저 (device가 만든 것이다)
    // 그래서 선언과 채우기를 뗀다.
    WindowSystem   windowSystem;   // 파괴: 마지막. glfwTerminate는 모든 창 뒤에
    VulkanInstance inst;
    VulkanDevice   dev;
    Window         window;         // swapchain을 품는다 -> dev보다 먼저 죽어야 한다
    Commands       commands;
    Descriptors    descriptors;   // frames가 이 pool에서 set을 받는다 -> 먼저 선언
    Frame          frames[kFramesInFlight];
    Pipeline       pipeline;
    Pipeline       wireframe;     // 같은 정점을 선으로. pipeline이 갈리는 첫 사례
    Pipeline       translucent;   // blend 켬 + depth write 끔
    Pipeline       fullscreen;
    Buffer         vertexBuffer;   // 파괴: 첫 번째

    // 채우기 - 의존 순서로. 조기 return이 아무것도 안 샌다.
    // ========================================================================

    // glfwInit이 먼저인 것은 의존 때문이 아니다. windowSystem이 맨 먼저 선언돼 있어서
    // (= 가장 늦게 죽는다) 생성 순서를 거기 맞췄다.
    if (!InitWindowSystem(&windowSystem)) { return 1; }
    if (!CreateInstance(&inst)) { return 1; }
    if (!OpenWindow(inst, 1280, 720, "Lambda Engine", &window)) { return 1; }

    // GPU를 고르고, GPU에게 물어볼 것을 다 묻는다.
    //
    // Format 둘 다 logical device가 필요 없다(이유는 각 함수 헤더에). 나란히 둔 이유는
    // 맞춰야 할 format이 둘이기 때문이다:
    //   formats                우리 render target이 쓸 것
    //   window.surfaceFormat   swapchain이 쓸 것 - present pass가 여기 맞춘다
    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    const RenderTargetFormats formats = ChooseRenderTargetFormats(inst, selection.gpu);
    if (formats.depth == VK_FORMAT_UNDEFINED) {
        LOG("[vk] no usable depth format\n");
        return 1;
    }
    if (!SelectSurfaceFormat(inst, selection.gpu, &window)) { return 1; }

    // selection은 여기서 dev 안으로 흡수된다.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }
    if (!CreateCommands(dev, &commands)) { return 1; }
    if (!CreateDescriptors(dev, kFramesInFlight, &descriptors)) { return 1; }

    for (Frame& f : frames) {
        if (!CreateFrame(dev, commands, descriptors, formats, &f)) { return 1; }
    }

    // 맞추는 상대가 다르다: scene은 우리 render target에, present는 swapchain에 그린다.
    if (!CreateTrianglePipeline(dev, formats, VK_POLYGON_MODE_FILL,
                                Blending::Opaque, &pipeline)) { return 1; }
    if (!CreateTrianglePipeline(dev, formats, VK_POLYGON_MODE_LINE,
                                Blending::Opaque, &wireframe)) { return 1; }
    if (!CreateTrianglePipeline(dev, formats, VK_POLYGON_MODE_FILL,
                                Blending::Translucent, &translucent)) { return 1; }
    if (!CreateFullscreenPipeline(dev, window.surfaceFormat.format,
                                  descriptors.setLayout, &fullscreen)) {
        return 1;
    }

    // 삼각형 둘을 겹치게 두고 그리는 순서를 깊이 순서와 반대로 만들었다. Depth test가
    // 실제로 도는지 보는 방법이다 - 겹친 곳이 초록이면 켜진 것이고 빨강이면 꺼진 것이다.
    //
    // **좌표가 이제 world다.** 전에는 z가 0.25/0.75, 즉 NDC 깊이값이라 그냥 숫자였다.
    // 이제는 카메라(z=2)로부터의 거리이고, 그래서 **둘을 같은 크기로 적었는데도 먼 쪽이
    // 작게 보인다** (2/3.5 = 0.57배). 원근이 실제로 도는지 보는 방법이다 -
    // 전에는 z만 다르고 크기가 같았다.
    constexpr Vertex kTriangles[] = {
        // 가까움 (z=0, 카메라에서 2), 먼저 그린다 - 초록
        {{-0.7f,  0.5f,  0.0f}, {0.1f, 0.9f, 0.2f}},
        {{-0.7f, -0.5f,  0.0f}, {0.1f, 0.9f, 0.2f}},
        {{ 0.3f,  0.0f,  0.0f}, {0.1f, 0.9f, 0.2f}},

        // 멈 (z=-1.5, 카메라에서 3.5), 나중에 그린다 - 빨강
        {{ 0.7f,  0.5f, -1.5f}, {0.9f, 0.2f, 0.1f}},
        {{-0.3f,  0.0f, -1.5f}, {0.9f, 0.2f, 0.1f}},
        {{ 0.7f, -0.5f, -1.5f}, {0.9f, 0.2f, 0.1f}},

        // 제일 가까움 (z=0.5, 카메라에서 1.5) - 반투명 파랑.
        // 앞의 둘과 같은 순서로 적는다(y-up 기준 CCW). 뒤집으면 culling에 잘린다.
        {{-0.2f,  0.6f,  0.5f}, {0.2f, 0.3f, 0.95f}},
        {{-0.2f, -0.4f,  0.5f}, {0.2f, 0.3f, 0.95f}},
        {{ 0.8f,  0.1f,  0.5f}, {0.2f, 0.3f, 0.95f}},
    };
    if (!CreateVertexBuffer(dev, commands, kTriangles, sizeof(kTriangles), &vertexBuffer)) {
        return 1;
    }

    // Swapchain은 여기서 안 만든다. 루프의 EnsureSwapchain이 만들고 최초 생성도
    // 재생성과 같은 경로다 - "지금 그릴 곳이 없다"가 시작 시점에도 정상이기 때문이다.

    // Frame을 여닫는 것(EnsureSwapchain · wait · acquire · submit · present)은
    // Frame.cpp에, 무엇을 그리는지는 RecordFrame에 있다. 여기 남은 것은 그 순서와
    // 실패했을 때 무엇을 할지뿐이다.
    LOG("close the window to exit.\n");

    // 어느 frame 자원 한 벌을 쓸 차례인가.
    uint32_t frameIndex = 0;

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // 최소화 중이면 event가 올 때까지 잔다. 이유는 WindowHasDrawableSize 주석에.
        if (!WindowHasDrawableSize(window)) {
            glfwWaitEvents();
            continue;
        }

        const Frame& frame = frames[frameIndex];

        FrameTarget target;
        const FrameResult begun = BeginFrame(dev, &window, frame, &target);
        if (begun == FrameResult::Fatal) { break; }
        if (begun == FrameResult::Skip) { continue; }

        // 아래 셋은 continue가 아니라 break다. acquire까지 갔는데 제출을 안 하면
        // 신호된 세마포어와 리셋된 펜스를 기다릴 사람이 없어진다.
        if (!RecordFrame(dev.table, frame.cmd, target, pipeline, wireframe,
                         translucent, vertexBuffer, fullscreen)) {
            break;
        }

        // Submit과 present가 갈라진 근거는 Frame.h에. 창이 없으면 아래 둘째 줄만 빠진다.
        if (!SubmitFrame(dev, frame, target.present->renderFinished)) {
            break;
        }
        if (!PresentFrame(dev, &window, *target.present)) {
            break;
        }

        frameIndex = (frameIndex + 1) % kFramesInFlight;
    }

    // 파괴는 소멸자가 선언의 역순으로 한다. 남은 한 줄은 GPU 대기다 - ~VulkanDevice도
    // vkDeviceWaitIdle을 부르지만 그건 다른 소멸자가 다 돈 뒤라서, GPU가 아직 쓰고
    // 있을 수 있는 것들(swapchain · command pool)을 부수기 전에 한 번 기다려야 한다.
    dev.table.vkDeviceWaitIdle(dev.handle);

    LOG("[vk] clean shutdown\n");
    return 0;
}
