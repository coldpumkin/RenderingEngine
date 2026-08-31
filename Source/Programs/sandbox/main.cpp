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
#include "Vulkan/Texture.h"
#include "Vulkan/Window.h"

#include <GLFW/glfw3.h>

#include <cstring>    // memcpy
#include <iterator>   // std::size

// 필요한 것만 하나씩 include한다. <glm/ext.hpp>는 벤더링할 때 뺐다 (VERSION.md).
#include <glm/common.hpp>                  // clamp
#include <glm/ext/matrix_clip_space.hpp>   // perspective
#include <glm/ext/matrix_transform.hpp>    // rotate · lookAt
#include <glm/geometric.hpp>               // normalize · cross
#include <glm/trigonometric.hpp>           // radians · cos · sin

// Frame 기록
// ============================================================================
//
// 여기 나오는 것들의 관계 - 셋으로 갈린다.
//
//   서로 맞아야 하는 것       format · pipeline · vertex buffer · descriptor set
//   명령이 적히는 곳          command buffer
//   동시에 여러 개 돌리는 것  frame
//
// Command buffer와 frame은 "무엇을 그리나"에 아무 말도 안 한다. 앞은 명령을 적는
// 테이프고, 뒤는 그 한 벌을 kFramesInFlight개 둬서 CPU가 앞서가게 하는 장치다.
//
// 맞아야 하는 것은 셋이고 pipeline이 그 가운데에 있다:
//   pipeline <-> render target   format (dynamic rendering이 pipeline에 박는다)
//   pipeline <-> vertex buffer   vertex layout (attribute description <-> Vertex)
//   pipeline <-> descriptor set  setLayout (pipeline layout이 참조한다. texture가
//                                들어오면서 셋이 됐다 - Descriptors.h)
//
// Pass는 vkCmdBeginRendering ~ vkCmdEndRendering 구간이고 그동안 그릴 대상이
// 고정된다. 한 frame에 pass가 둘이고 뒤가 앞의 결과를 읽는다:
//
//   [Scene Pass]  창을 모른다
//     barrier x2 (우리 color·depth)
//     +- BeginRendering --- attachment = draw.color / draw.depth
//     |    BindVertexBuffers   layout이 pipeline과 같아야 한다
//     |    BindIndexBuffer     UINT16
//     |    for item:
//     |      BindPipeline      **바뀔 때만.** 물체 5개에 bind 3번
//     |      PushConstants     mvp + alpha
//     |      DrawIndexed
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

// Index buffer 안의 한 구간
//
// 전에는 이 둘이 DrawItem의 필드로 풀려 있었고, 값은 index 배열의 자리를 손으로 다시
// 적은 숫자였다(`{9, 6}`). 배열과 DrawItem이 100줄 떨어져 있어서 index를 하나만
// 끼워 넣어도 조용히 어긋났다 - 검증 레이어도 컴파일러도 못 잡는다.
//
// 타입을 만든 것만으로는 안 고쳐진다. 고치는 것은 **이름 붙은 상수를 index 배열
// 바로 옆에 두는 것**이고, 이 타입은 그 이름이 한 덩어리로 넘어가게 한다.
struct IndexRange {
    uint32_t firstIndex = 0;
    uint32_t count = 0;

    // 다음 구간이 여기서 시작한다. 상수끼리 이어서 자리를 유도한다.
    constexpr uint32_t End() const noexcept { return firstIndex + count; }
};

// 한 번의 draw에 필요한 것 전부
// ============================================================================
//
// **물체가 아니다.** 어떤 움직임에서 나온 행렬인지, 어느 물체의 index인지는 여기
// 안 남는다. 물체마다 따로 적혀 있던 코드를 배열 하나로 fold하면서 실제로 무엇이
// 달랐는지가 그대로 필드가 됐다.
//
// pipeline과 texture가 값이 아니라 handle/포인터인 이유: 여러 item이 같은 것을
// 가리키고, loop가 **바뀔 때만** bind한다.
//
// **둘이 따로 있는 것이 이 struct가 말하는 전부다.** texture가 하나였을 때는
// pass 앞에서 한 번 bind하고 끝이라 필드가 없었다. 둘이 되자 물체마다 정해지는
// 것이 되었고, 그러면서 pipeline과 **다른 순서로** 바뀐다는 것이 드러났다 -
// 지금 물체 다섯에 pipeline bind가 셋, texture bind가 넷이다. 축이 둘이다.
struct DrawItem {
    const Pipeline* pipeline = nullptr;
    VkDescriptorSet texture = VK_NULL_HANDLE;
    PushConstants push{};        // mvp + alpha. 둘 다 물체마다 정해진다
    IndexRange range{};
};

// Scene Pass
//
// Input:  cmd, draw, vertex/index buffer, 그릴 것 목록
// Effect: draw.color / draw.depth에 그리는 명령이 cmd에 append된다
//
// Swapchain이 인자에 없다 - 창이 없어도 성립한다.
//
// **카메라도 시간도 안 받는다.** 전에는 여기서 glfwGetTime과 lookAt/perspective를
// 직접 불렀는데, 그건 장면의 상태지 기록의 일이 아니다. 지금 이 함수가 아는 것은
// "이 행렬로 이 index 구간을 이 pipeline과 이 texture로 그려라"뿐이다.
//
// **textureSet 인자가 사라졌다.** 물체마다 다른 texture를 쓰게 되면서 그것이
// pass 전체의 성질이 아니라 draw마다의 성질이 됐고, DrawItem 안으로 들어갔다.
static void RecordScenePass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                            const RenderTargets& draw,
                            const Buffer& vertexBuffer,
                            const Buffer& indexBuffer,
                            const DrawItem* items, uint32_t itemCount) noexcept {
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

    // 그릴 것이 없으면 clear만 하고 나간다. viewport를 items[0]에서 얻으므로
    // 여기서 걸러야 한다.
    if (itemCount == 0) {
        vk.vkCmdEndRendering(cmd);
        return;
    }

    // Viewport/scissor를 dynamic state로 둔 덕에 창 크기가 바뀌어도 pipeline을 다시
    // 만들 필요가 없다.
    //
    // 부호를 여기서 안 정한다. pipeline이 든 값을 그대로 넘기므로 그쪽의 frontFace와
    // 어긋날 수가 없다 (Pipeline.h). 여기는 y-up이라 뒤집혀 나온다.
    //
    // items[0]에서 얻는다 - scene pipeline이 전부 같은 y 규약을 쓴다는 전제다.
    // 규약이 갈리는 pipeline이 섞이면 이 두 줄이 loop 안으로 들어가야 한다
    // (dynamic state라 들어갈 수 있다).
    const VkViewport viewport = MakeViewport(extent, items[0].pipeline->viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);

    // 시저: 이 사각형 밖의 픽셀은 버린다. 지금은 화면 전체다.
    VkRect2D scissor{};
    scissor.extent = extent;
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    // binding 0은 pipeline의 binding=0과 짝이다. offset은 buffer 안의 시작 바이트 -
    // 여러 mesh를 한 buffer에 담으면 여기가 달라진다.
    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.handle, &offset);

    // Index buffer는 slot이 없다. vertex는 binding 번호가 있는데 index는 하나뿐이라
    // "지금 쓰는 index buffer"가 command buffer에 하나만 있다.
    //
    // UINT16은 vertex가 65536개 미만일 때 쓴다. 넘으면 UINT32로 바꿔야 하고,
    // **그때 이 인자와 kIndices의 타입이 같이 움직여야 한다.**
    vk.vkCmdBindIndexBuffer(cmd, indexBuffer.handle, 0, VK_INDEX_TYPE_UINT16);

    // 물체마다 따로 적혀 있던 코드를 fold한 결과다. 남은 것은 순서뿐이고,
    // 순서는 호출자가 배열에 적은 그대로다 - 반투명이 마지막이어야 한다는 규칙도
    // 이제 이 함수가 아니라 배열을 만드는 쪽이 진다.
    //
    // **bind가 둘로 늘었고 서로 독립이다.** texture가 하나였을 때는 pass 앞에서
    // 한 번 bind하고 끝이었다. 둘이 되자 pipeline과 같은 모양의 "바뀔 때만" 논리가
    // 하나 더 생겼는데, **바뀌는 자리가 서로 다르다** - 지금 pipeline 3번,
    // texture 4번이다. 순서를 한쪽에 맞추면 다른 쪽이 손해를 본다.
    const Pipeline* boundPipeline = nullptr;
    VkDescriptorSet boundTexture = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < itemCount; ++i) {
        const DrawItem& item = items[i];

        // **바뀔 때만 bind한다.** 같은 pipeline이 이어지면 명령이 안 나간다.
        if (item.pipeline != boundPipeline) {
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 item.pipeline->handle);
            boundPipeline = item.pipeline;
        }

        // **pipeline이 바뀌어도 set이 안 풀린다.** scene pipeline 셋이 layout 정의가
        // 같아서(같은 push range + 같은 setLayout) 호환되기 때문이다. 그래서 위
        // BindPipeline 뒤에 다시 bind하지 않아도 되고, 이 조건이 pipeline 조건과
        // 따로 설 수 있다. layout이 갈리는 pipeline이 섞이면 이 전제가 깨진다.
        if (item.texture != boundTexture) {
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       item.pipeline->layout, 0, 1, &item.texture,
                                       0, nullptr);
            boundTexture = item.texture;
        }

        vk.vkCmdPushConstants(cmd, item.pipeline->layout,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(item.push), &item.push);

        // vkCmdDraw와 인자가 다르다. firstIndex는 **index buffer 안의 위치**이고,
        // vertexOffset(0)은 그 index에 더해지는 값이다 - mesh마다 vertex를 0부터
        // 세고 싶을 때 쓴다. 지금은 index에 절대 번호를 적어서 0이다.
        vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex, 0, 0);
    }

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

// Input:  cmd, target, vertex/index buffer, 그릴 것 목록, fullscreen
// Effect: cmd를 리셋하고 pass 둘을 기록한다
// Output: false면 cmd가 무효 상태다 - 제출하면 안 된다
bool RecordFrame(const VolkDeviceTable& vk,
                 VkCommandBuffer cmd,
                 const FrameTarget& target,
                 const Buffer& vertexBuffer,
                 const Buffer& indexBuffer,
                 const DrawItem* items, uint32_t itemCount,
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

    RecordScenePass(vk, cmd, *target.draw, vertexBuffer, indexBuffer,
                    items, itemCount);
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
    Texture        checker;
    Texture        stripe;        // 물체마다 다른 texture를 붙이려고 둘째가 생겼다
    Buffer         vertexBuffer;
    Buffer         indexBuffer;    // 파괴: 첫 번째

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
    // maxSets: frame마다 render target set 하나 + texture마다 하나.
    // pool은 자라지 않아서 여기를 안 늘리면 set 할당이 실패한다.
    //
    // **texture가 둘이 되면서 이 값을 두 번째로 손으로 고쳤다.** 전에는 `+ 1`이라고
    // 적혀 있었다 - 소비자 개수를 여기서 세는 구조라 소비자가 늘 때마다 여기가
    // 같이 안 움직이면 조용히 모자란다(실패는 하니 늦게라도 터진다).
    // 이름을 붙여 등급을 낮췄다. 위 Texture 선언 개수와 같아야 한다.
    //
    // 진짜로 유도하려면 texture들이 배열이 되어야 하는데, 그러면 checker/stripe라는
    // 이름이 index로 바뀌어 더 나빠진다. **texture에 이름 말고 다른 구분이 생길 때**
    // (= 이름이 아니라 material id로 고르게 될 때) 그 교환이 이득이 된다.
    constexpr uint32_t kTextureCount = 2;
    if (!CreateDescriptors(dev, kFramesInFlight + kTextureCount, &descriptors)) { return 1; }

    for (Frame& f : frames) {
        if (!CreateFrame(dev, commands, descriptors, formats, &f)) { return 1; }
    }

    // 맞추는 상대가 다르다: scene은 우리 render target에, present는 swapchain에 그린다.
    if (!CreateTrianglePipeline(dev, formats, descriptors.setLayout,
                                VK_POLYGON_MODE_FILL, Blending::Opaque, &pipeline)) { return 1; }
    if (!CreateTrianglePipeline(dev, formats, descriptors.setLayout,
                                VK_POLYGON_MODE_LINE, Blending::Opaque, &wireframe)) { return 1; }
    if (!CreateTrianglePipeline(dev, formats, descriptors.setLayout,
                                VK_POLYGON_MODE_FILL, Blending::Translucent, &translucent)) { return 1; }
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
    // uv는 **y가 아래로 간다** - world는 y-up이라 위쪽 정점이 v=0이다.
    constexpr Vertex kTriangles[] = {
        // 가까움 (z=0, 카메라에서 2), 먼저 그린다 - 초록
        {{-0.7f,  0.5f,  0.0f}, {0.1f, 0.9f, 0.2f}, {0.0f, 0.0f}},
        {{-0.7f, -0.5f,  0.0f}, {0.1f, 0.9f, 0.2f}, {0.0f, 1.0f}},
        {{ 0.3f,  0.0f,  0.0f}, {0.1f, 0.9f, 0.2f}, {1.0f, 0.5f}},

        // 멈 (z=-1.5, 카메라에서 3.5), 나중에 그린다 - 빨강
        {{ 0.7f,  0.5f, -1.5f}, {0.9f, 0.2f, 0.1f}, {1.0f, 0.0f}},
        {{-0.3f,  0.0f, -1.5f}, {0.9f, 0.2f, 0.1f}, {0.0f, 0.5f}},
        {{ 0.7f, -0.5f, -1.5f}, {0.9f, 0.2f, 0.1f}, {1.0f, 1.0f}},

        // 제일 가까움 (z=0.5, 카메라에서 1.5) - 반투명 파랑.
        // 앞의 둘과 같은 순서로 적는다(y-up 기준 CCW). 뒤집으면 culling에 잘린다.
        {{-0.2f,  0.6f,  0.5f}, {0.2f, 0.3f, 0.95f}, {0.0f, 0.0f}},
        {{-0.2f, -0.4f,  0.5f}, {0.2f, 0.3f, 0.95f}, {0.0f, 1.0f}},
        {{ 0.8f,  0.1f,  0.5f}, {0.2f, 0.3f, 0.95f}, {1.0f, 0.5f}},
    };
    // Quad. **정점 4개로 삼각형 2개를 그린다** - index buffer가 처음으로 값을 하는
    // 자리다. 정점 둘(kQuadBase+0, +2)이 두 번씩 쓰인다.
    // 위 셋과 같은 순서로 적는다(y-up 기준 CCW).
    constexpr Vertex kQuad[] = {
        {{-1.4f,  0.45f, -0.8f}, {0.95f, 0.75f, 0.15f}, {0.0f, 0.0f}},
        {{-1.4f, -0.45f, -0.8f}, {0.95f, 0.75f, 0.15f}, {0.0f, 1.0f}},
        {{-0.5f, -0.45f, -0.8f}, {0.95f, 0.75f, 0.15f}, {1.0f, 1.0f}},
        {{-0.5f,  0.45f, -0.8f}, {0.95f, 0.75f, 0.15f}, {1.0f, 0.0f}},
    };

    Vertex vertices[std::size(kTriangles) + std::size(kQuad)]{};
    std::memcpy(vertices, kTriangles, sizeof(kTriangles));
    std::memcpy(vertices + std::size(kTriangles), kQuad, sizeof(kQuad));

    // Index buffer. 삼각형 셋은 정점을 그대로 한 번씩 가리키고(재사용 없음),
    // quad만 정점 둘을 두 번 가리킨다.
    //
    // **타입이 vkCmdBindIndexBuffer의 VK_INDEX_TYPE_UINT16과 짝이다.** 어긋나면
    // 컴파일도 실행도 되는데 엉뚱한 정점이 나온다 - 검증 레이어도 못 잡는다.
    //
    // kQuadBase는 위 memcpy가 만든 자리다. 전에는 여기에 9라고 적혀 있었는데, 그건
    // std::size(kTriangles)를 손으로 옮겨 적은 값이라 삼각형을 하나 더 넣으면 quad가
    // 조용히 엉뚱한 정점을 가리켰다.
    constexpr uint16_t kQuadBase = static_cast<uint16_t>(std::size(kTriangles));
    constexpr uint16_t kIndices[] = {
        0, 1, 2,          // 초록 삼각형
        3, 4, 5,          // 빨강 삼각형
        6, 7, 8,          // 파랑 삼각형 (반투명)
        kQuadBase + 0, kQuadBase + 1, kQuadBase + 2,   // quad 앞쪽 절반
        kQuadBase + 2, kQuadBase + 3, kQuadBase + 0,   // quad 뒤쪽 절반 - 두 정점을 다시 쓴다
    };

    // 위 배열의 어느 구간이 어느 물체인가. **선언 순서가 kIndices의 순서와 같다** -
    // 각자가 앞의 것이 끝난 자리에서 시작하므로 중간에 하나를 끼워 넣으면 뒤가 전부
    // 따라 움직인다. 남은 것은 count뿐이고, 그건 물체의 성질이다(삼각형 3, quad 6).
    //
    // 이것이 DrawItem 쪽에 자리 숫자가 안 남게 하는 유일한 장치다. 여기와 kIndices가
    // 붙어 있는 것 자체가 그 장치의 절반이다 - 떨어뜨리면 다시 어긋난다.
    constexpr IndexRange kGreenIndices{0, 3};
    constexpr IndexRange kRedIndices{kGreenIndices.End(), 3};
    constexpr IndexRange kBlueIndices{kRedIndices.End(), 3};
    constexpr IndexRange kQuadIndices{kBlueIndices.End(), 6};
    static_assert(kQuadIndices.End() == std::size(kIndices),
                  "구간의 합이 index 배열을 다 덮지 않는다");

    if (!CreateDeviceLocalBuffer(dev, commands, vertices, sizeof(vertices),
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertexBuffer)) {
        return 1;
    }
    if (!CreateDeviceLocalBuffer(dev, commands, kIndices, sizeof(kIndices),
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indexBuffer)) {
        return 1;
    }

    if (!CreateCheckerTexture(dev, commands, descriptors, &checker)) { return 1; }
    if (!CreateStripeTexture(dev, commands, descriptors, &stripe)) { return 1; }

    // Swapchain은 여기서 안 만든다. 루프의 EnsureSwapchain이 만들고 최초 생성도
    // 재생성과 같은 경로다 - "지금 그릴 곳이 없다"가 시작 시점에도 정상이기 때문이다.

    // Frame을 여닫는 것(EnsureSwapchain · wait · acquire · submit · present)은
    // Frame.cpp에, 무엇을 그리는지는 RecordFrame에 있다. 여기 남은 것은 그 순서와
    // 실패했을 때 무엇을 할지뿐이다.
    LOG("close the window to exit.\n");

    // Projection - 주기가 frame이 아니라 render target의 수명이다
    // ========================================================================
    //
    // 전에는 이 계산이 루프 안에 있었고 target.draw->extent에서 aspect를 얻었다.
    // 그런데 그 extent는 Config.h의 컴파일 상수라(Frame.cpp가 그 값으로 만든다)
    // 어느 frame이든 어떻게 리사이즈하든 같은 값이었다 - 매 frame 다시 만들고 있었다.
    //
    // **창 크기가 아니다.** 창은 present pass가 쓰고, 여기는 우리 render target이다.
    // 그래서 리사이즈로는 안 변한다.
    //
    // 이 값이 변하는 사건은 하나뿐이다: 렌더 해상도가 런타임 값이 되는 것. 그때는
    // render target 재생성 경로가 같이 생기고(Config.h가 그 조건을 적어놨다)
    // 이 계산이 프레임이 아니라 **그 경로로** 따라간다.
    //
    // frames[0]에서 얻는다 - 전부 같은 크기로 만들어진다. 갈라지면 여기가 못 쓴다.
    const VkExtent2D sceneExtent = frames[0].targets.extent;
    const float aspect = static_cast<float>(sceneExtent.width)
                       / static_cast<float>(sceneExtent.height);

    // proj[1][1]에 -1을 곱하지 않는다 - viewport height가 이미 음수다.
    // 깊이가 [0,1]로 나오는 것은 GLM_FORCE_DEPTH_ZERO_TO_ONE 덕이고 CMake의 glm
    // 타깃에 붙어 있다. near를 0.1로 잡았다 (깊이 정밀도는 near 근처에 몰린다).
    const glm::mat4 proj =
        glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);

    // 프레임 사이를 넘어가는 상태 - 여기 있는 것이 전부다
    // ========================================================================
    //
    // 루프 안의 값들은 전부 매 frame 새로 만들어진다. 넘어가는 것은 아래뿐이고,
    // 카메라가 들어오기 전에는 frameIndex 하나였다.
    //
    // 셋을 struct로 안 묶었다. 관계는 있지만(같이 view를 만든다) 붙어 있고, 읽는
    // 곳이 lookAt 한 군데뿐이라 묶어도 강제되는 것이 없다. 갈리는 자리: 카메라를
    // 읽는 두 번째 소비자가 생길 때 - frustum culling · 조명의 시점 · shadow pass.

    // 어느 frame 자원 한 벌을 쓸 차례인가.
    uint32_t frameIndex = 0;

    // 시계. dt가 필요해서 마지막으로 읽은 시각을 들고 있어야 한다.
    double lastTime = glfwGetTime();

    // 카메라. yaw = -90도면 -z를 본다 (아래 forward 식에 넣어보면 (0,0,-1)이다) -
    // 전에 lookAt에 박아뒀던 시점과 같은 자리에서 시작한다.
    glm::vec3 eye{0.0f, 0.0f, 2.0f};
    float yaw = -90.0f;
    float pitch = 0.0f;

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // 최소화 중이면 event가 올 때까지 잔다. 이유는 WindowHasDrawableSize 주석에.
        //
        // **깨어난 뒤 시계를 다시 맞춘다.** 안 맞추면 잔 시간이 그대로 다음 dt가 되고
        // (10초 자면 dt=10초) 복귀하는 순간 카메라가 순간이동한다. 잔 것은 frame이
        // 아니라서 시간을 버리는 쪽이 맞다 - 아래 Skip과 반대다.
        if (!WindowHasDrawableSize(window)) {
            glfwWaitEvents();
            lastTime = glfwGetTime();
            continue;
        }

        const Frame& frame = frames[frameIndex];

        FrameTarget target;
        const FrameResult begun = BeginFrame(dev, &window, frame, &target);
        if (begun == FrameResult::Fatal) { break; }

        // **여기는 시계를 안 건드린다.** 최소화와 반대다 - Skip은 짧고(swapchain
        // 재생성 한 번) 그동안 시간이 실제로 흘렀다. 버리면 리사이즈 드래그 중에
        // 카메라가 멈췄다가 튄다.
        if (begun == FrameResult::Skip) { continue; }

        // Surface format이 바뀌었으면 fullscreen pipeline을 다시 만든다
        // --------------------------------------------------------------------
        //
        // **왜 이 자리인가.** 플래그는 BeginFrame 안의 EnsureSwapchain이 세운다.
        // 루프 맨 앞에서 보면 한 바퀴 늦어서, 바뀐 그 frame을 옛 pipeline으로 그린다.
        //
        // Scene pipeline 셋은 안 건드린다 - 그쪽 format은 우리 render target 것이라
        // surface와 무관하다. 여기 걸리는 것은 fullscreen 하나뿐이다.
        //
        // 아래 waitIdle은 DestroyPipeline의 Contract를 지키는 것이다 (Pipeline.h).
        // BeginFrame이 기다린 것은 이 frame의 fence 하나뿐이라 그것으로는 부족하다.
        if (window.surfaceFormatChanged) {
            dev.table.vkDeviceWaitIdle(dev.handle);
            DestroyPipeline(dev, &fullscreen);
            if (!CreateFullscreenPipeline(dev, window.surfaceFormat.format,
                                          descriptors.setLayout, &fullscreen)) {
                break;
            }
            // 세운 쪽이 아니라 처리한 쪽이 지운다.
            window.surfaceFormatChanged = false;
        }

        // 이번 frame에 그릴 것을 여기서 만든다
        // --------------------------------------------------------------------
        //
        // 전에는 이 계산이 RecordScenePass 안에 있었다. 시간도 카메라도 물체도
        // 장면의 상태지 기록의 일이 아니라 위로 올렸다. 기록 쪽으로 내려가는 것은
        // DrawItem 배열 하나뿐이고, 그 안에는 Transform도 Geometry도 안 남는다 -
        // 카메라와 곱해진 행렬 하나, index 구간, pipeline이 전부다.
        //
        // **여기 남는 기준은 "frame마다 변하는가"다.** aspect와 proj는 그렇지 않아서
        // 루프 밖으로 나갔다(위 주석). 남은 것은 시계와 그것으로 만드는 것들이다.

        // 시계를 한 번만 읽어 값 둘을 만든다.
        //   t   절대 시간. 물체 회전이 쓴다 (누적이 아니라 시각의 함수다)
        //   dt  지난 frame과의 간격. 카메라 이동이 쓴다 (속도 x dt를 누적한다)
        // 둘이 같은 읽기에서 나오므로 서로 어긋날 수 없다.
        const double now = glfwGetTime();
        const float t = static_cast<float>(now);
        const float dt = static_cast<float>(now - lastTime);
        lastTime = now;

        // 입력 -> 카메라 상태
        // --------------------------------------------------------------------
        //
        // glfwGetKey는 폴링이다. 값은 위 glfwPollEvents가 갱신해둔 것이라, 이 블록이
        // BeginFrame 앞이든 뒤든 같은 값을 준다. callback을 안 쓴 이유: 리사이즈는
        // "언제 일어났나"가 중요한 event지만 이동은 "지금 눌려 있나"가 전부다.
        //
        // 속도에 dt를 곱한다. 안 곱하면 frame rate가 곧 속도가 된다.
        constexpr float kMoveSpeed = 2.0f;    // world 단위 / 초
        constexpr float kTurnSpeed = 90.0f;   // 도 / 초

        const auto held = [&](int key) {
            return glfwGetKey(window.handle, key) == GLFW_PRESS;
        };

        if (held(GLFW_KEY_LEFT))  { yaw   -= kTurnSpeed * dt; }
        if (held(GLFW_KEY_RIGHT)) { yaw   += kTurnSpeed * dt; }
        if (held(GLFW_KEY_UP))    { pitch += kTurnSpeed * dt; }
        if (held(GLFW_KEY_DOWN))  { pitch -= kTurnSpeed * dt; }

        // +-90도에서 forward가 world up과 나란해지고 cross가 0 벡터가 된다.
        // 그러면 right를 normalize할 때 0으로 나눈다.
        pitch = glm::clamp(pitch, -89.0f, 89.0f);

        // yaw/pitch -> 방향. yaw=-90, pitch=0을 넣으면 (0,0,-1)이다.
        const glm::vec3 forward = glm::normalize(glm::vec3{
            glm::cos(glm::radians(yaw)) * glm::cos(glm::radians(pitch)),
            glm::sin(glm::radians(pitch)),
            glm::sin(glm::radians(yaw)) * glm::cos(glm::radians(pitch)),
        });

        // world up으로 유도한다 - forward와 같이 움직이므로 따로 들면 어긋난다.
        constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
        const glm::vec3 right = glm::normalize(glm::cross(forward, kWorldUp));

        if (held(GLFW_KEY_W)) { eye += forward * kMoveSpeed * dt; }
        if (held(GLFW_KEY_S)) { eye -= forward * kMoveSpeed * dt; }
        if (held(GLFW_KEY_D)) { eye += right   * kMoveSpeed * dt; }
        if (held(GLFW_KEY_A)) { eye -= right   * kMoveSpeed * dt; }
        if (held(GLFW_KEY_E)) { eye += kWorldUp * kMoveSpeed * dt; }
        if (held(GLFW_KEY_Q)) { eye -= kWorldUp * kMoveSpeed * dt; }

        // **proj와 달리 이건 루프 안에 남는다.** 이제 실제로 frame마다 변한다 -
        // 전에는 "입력이 들어오면 변한다"는 예고였고 값은 죽어 있었다.
        //
        // center를 eye + forward로 준다. 절대 좌표를 주면 카메라가 움직일 때
        // 시선이 그 점에 묶여서 회전이 안 된다.
        const glm::mat4 view = glm::lookAt(eye, eye + forward, kWorldUp);
        const glm::mat4 camera = proj * view;

        // **순서가 규칙이고, 이제 그 규칙이 셋이다.**
        //   반투명은 맨 뒤     depth write를 껐으므로 depth가 순서를 안 지켜준다
        //   같은 pipeline끼리  붙어 있으면 BindPipeline이 준다
        //   같은 texture끼리   붙어 있으면 BindDescriptorSets가 준다
        //
        // **앞의 하나는 정확성이고 뒤의 둘은 비용이다.** 그리고 뒤의 둘이 서로
        // 다른 순서를 원한다 - 지금 배열은 pipeline으로 묶여 있어서 pipeline bind가
        // 3번, texture bind가 4번이다. texture로 묶으면 반대가 된다.
        //
        // 어느 쪽을 우선할지는 재봐야 아는 것이고, 지금은 물체가 다섯이라 잴 것이
        // 없다. **그 선택이 sort key가 생기는 자리다** - 물체가 수백이 될 때.
        const glm::vec3 kZAxis{0.0f, 0.0f, 1.0f};
        const DrawItem items[] = {
            // 초록. 원점에서 z축 회전 (축이 z라 깊이가 안 바뀐다)
            {&pipeline, checker.set,
             {camera * glm::rotate(glm::mat4(1.0f), t, kZAxis), 1.0f}, kGreenIndices},

            // 빨강. 반대 방향으로 더 천천히. **여기서 texture가 갈린다**
            {&pipeline, stripe.set,
             {camera * glm::rotate(glm::mat4(1.0f), -t * 0.5f, kZAxis), 1.0f}, kRedIndices},

            // Quad. 안 움직인다 - Transform이 아무것도 아닐 수도 있다
            {&pipeline, checker.set, {camera, 1.0f}, kQuadIndices},

            // 초록을 한 번 더, 이번엔 선으로. **같은 구간을 가리킨다** -
            // 정점을 안 늘리고 물체만 늘었다
            {&wireframe, checker.set,
             {camera * glm::scale(
                  glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, -0.6f, -0.5f)),
                  glm::vec3(0.5f)), 1.0f}, kGreenIndices},

            // 반투명 파랑. 마지막이어야 한다
            {&translucent, stripe.set, {camera, 0.5f}, kBlueIndices},
        };

        // 아래 셋은 continue가 아니라 break다. acquire까지 갔는데 제출을 안 하면
        // 신호된 세마포어와 리셋된 펜스를 기다릴 사람이 없어진다.
        if (!RecordFrame(dev.table, frame.cmd, target, vertexBuffer, indexBuffer,
                         items, static_cast<uint32_t>(std::size(items)), fullscreen)) {
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
