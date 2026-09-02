// Flow lives here: what is created in what order, and how one frame runs.
//
// Vulkan/ holds how each resource is made and destroyed. This file holds order only.
//
// Init and runtime obey different rules, and that is what split the files:
//
//               init        runtime (per frame)
//   runs        once        hundreds per second
//   heap        free        forbidden
//   log         free        floods if the condition persists
//   failure     unwind      drop the frame or recover


#include "Config.h"
#include "Vulkan/Barrier.h"
#include "Vulkan/Commands.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Mesh.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Texture.h"
#include "Vulkan/Window.h"

#include <GLFW/glfw3.h>

#include <cstring>    // memcpy
#include <iterator>   // std::size

// One header at a time. <glm/ext.hpp> was dropped when vendoring (VERSION.md).
#include <glm/common.hpp>                  // clamp
#include <glm/ext/matrix_clip_space.hpp>   // perspective
#include <glm/ext/matrix_transform.hpp>    // rotate, lookAt
#include <glm/geometric.hpp>               // normalize, cross
#include <glm/trigonometric.hpp>           // radians, cos, sin

// Frame recording
// ============================================================================
//
// Three kinds of things appear below:
//
//   must agree      format . pipeline . vertex buffer . descriptor set
//   holds commands  command buffer
//   runs in flight  frame
//
// Pipeline sits in the middle of all three agreements:
//   pipeline <-> render target   attachment format (dynamic rendering bakes it in)
//   pipeline <-> vertex buffer   vertex layout
//   pipeline <-> descriptor set  set layout
//
// A pass is vkCmdBeginRendering..vkCmdEndRendering, and the draw target is fixed
// inside it. Two passes per frame, the second reads what the first wrote:
//
//   [scene pass]    knows nothing about the window
//     barrier x3 (our color, its resolve target, depth)
//     BeginRendering   attachment = draw.color / draw.depth, resolving into draw.resolve
//       BindVertexBuffers, BindIndexBuffer
//       BindPipeline, BindDescriptorSets
//       per item: PushConstants, DrawIndexed
//     EndRendering    <- the multisample average happens here
//
//   [present pass]  reads the resolve image only
//     barrier resolve   -> SHADER_READ_ONLY
//     barrier swapchain -> COLOR_ATTACHMENT
//     BeginRendering   attachment = swapchain image, in the swapchain's own format
//       BindPipeline, BindDescriptorSets, Draw 3 vertices
//     EndRendering
//     barrier swapchain -> PRESENT_SRC
//
// No fence, semaphore, acquire or present appears here. That lives in Frame.cpp,
// and recording and synchronization do not know about each other.

// A span inside the index buffer.
//
// Holding the two numbers together lets DrawItem carry a name instead of a position.
// The named spans live next to kIndices; separating them breaks the guard.
struct IndexRange {
    uint32_t firstIndex = 0;
    uint32_t count = 0;

    // Next span starts here, so spans chain instead of repeating positions.
    constexpr uint32_t End() const noexcept { return firstIndex + count; }
};

// What differs between draws, once the pass has fixed everything else.
//
// No pipeline, texture or camera: the pass holds one of each. Each moves in here the
// day one pass needs two of it, and the bind then moves into the loop with it.
struct DrawItem {
    glm::mat4 model{1.0f};
    float alpha = 1.0f;
    IndexRange range{};
};

// Scene pass
//
// Input:  the slot (target, pipeline), and what the scene brings: mesh, texture, items
// Effect: appends commands that draw into draw.color / draw.depth
//
// No swapchain, so this works without a window. Takes the camera because one pass
// has one viewpoint; time stays out -- item.model already carries it.
//
// One mesh for every item: the spans in items index into it. A second mesh means
// another BindVertexBuffers, which is why the bind sits above the loop and not in it.
static void RecordScenePass(const VolkDeviceTable& vk, const FrameSlot& slot,
                            const Mesh& mesh, VkDescriptorSet texture,
                            const glm::mat4& camera,
                            const DrawItem* items, uint32_t itemCount) noexcept {
    VkCommandBuffer cmd = slot.cmd;
    const RenderTargets& draw = slot.targets;
    const Pipeline& pipeline = *slot.scene;
    const VkExtent2D extent = draw.extent;   // render resolution, not window size

    // oldLayout UNDEFINED: loadOp=CLEAR overwrites, so the old contents are dead.
    // Asking to preserve them makes the driver actually copy.
    RecordLayoutTransition(vk, cmd, draw.color.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // The resolve target is written too, at the end of the pass, so it needs the same
    // layout and the same stage. Nothing here draws into it directly.
    RecordLayoutTransition(vk, cmd, draw.resolve.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Depth test runs at EARLY/LATE_FRAGMENT_TESTS, ahead of COLOR_ATTACHMENT_OUTPUT.
    // Reusing the color stage here would let depth writes pass the barrier.
    RecordLayoutTransition(vk, cmd, draw.depth.handle, VK_IMAGE_ASPECT_DEPTH_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                               | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    // imageView is the multisample image, resolveImageView is what survives the pass.
    // vkCmdEndRendering does the averaging, so there is no second pass and no
    // vkCmdResolveImage.
    //
    // storeOp DONT_CARE goes with that: only the resolved copy is read afterwards, so
    // writing the multisample image back would be pure bandwidth. The resolve still
    // happens -- resolveMode is what drives it, not storeOp.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = draw.color.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = draw.resolve.view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.clearValue.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    // Clear 1.0 = farthest, paired with the pipeline's compareOp=LESS.
    // DONT_CARE: depth is used only within this frame.
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

    // Dynamic state, so a resize does not rebuild the pipeline. The sign comes from the
    // pipeline itself, so it cannot disagree with the frontFace baked into it.
    const VkViewport viewport = MakeViewport(extent, pipeline.viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);

    // Pixels outside this rect are discarded. Whole screen for now.
    VkRect2D scissor{};
    scissor.extent = extent;
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
                               0, 1, &texture, 0, nullptr);

    // binding 0 matches the pipeline's binding 0. offset changes once several meshes
    // share one buffer.
    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.handle, &offset);

    // Index buffers have no slot number: a command buffer holds exactly one.
    // Contract: this type must match the element type of kIndices.
    vk.vkCmdBindIndexBuffer(cmd, mesh.indices.handle, 0, mesh.indexType);

    // Order is whatever the caller wrote into the array. This layer does not sort.
    // Nothing is bound in here, so the order only decides blending.
    for (uint32_t i = 0; i < itemCount; ++i) {
        const DrawItem& item = items[i];

        // The pass's viewpoint meets the item's transform here, and nowhere earlier.
        const PushConstants push{camera * item.model, item.alpha};
        vk.vkCmdPushConstants(cmd, pipeline.layout,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(push), &push);

        // firstIndex is a position in the index buffer. vertexOffset (0) is added to
        // every index, which matters once meshes number their vertices from zero.
        vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex, 0, 0);
    }

    vk.vkCmdEndRendering(cmd);
}

// Present pass
//
// Input:  the slot (what the scene pass wrote, and the set naming it), and where to put it
// Effect: appends commands that sample the resolve image into the swapchain image
static void RecordPresentPass(const VolkDeviceTable& vk, const FrameSlot& slot,
                              const AcquiredFrame& acquired) noexcept {
    VkCommandBuffer cmd = slot.cmd;
    const Image& source = slot.targets.resolve;
    const Pipeline& fullscreen = *slot.present;
    const SwapchainImage& present = *acquired.image;
    const VkExtent2D presentExtent = acquired.extent;
    // Written as an attachment, read as a texture -- that is this whole pass. The
    // layout must equal the one recorded into the descriptor set.
    RecordLayoutTransition(vk, cmd, source.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // srcStage must overlap SubmitFrame's wait stage, or this transition can run ahead
    // of the acquire.
    RecordLayoutTransition(vk, cmd, present.image, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Window sized, unlike the scene pass. The sampler's LINEAR filter scales.
    VkRenderingAttachmentInfo swapColor{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    swapColor.imageView = present.view;
    swapColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // the draw covers everything
    swapColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo presentPass{VK_STRUCTURE_TYPE_RENDERING_INFO};
    presentPass.renderArea.extent = presentExtent;
    presentPass.layerCount = 1;
    presentPass.colorAttachmentCount = 1;
    presentPass.pColorAttachments = &swapColor;

    vk.vkCmdBeginRendering(cmd, &presentPass);

    // Opposite sign from the scene pass: this pipeline is built ViewportY::Down.
    const VkViewport presentViewport = MakeViewport(presentExtent, fullscreen.viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &presentViewport);

    VkRect2D presentScissor{};
    presentScissor.extent = presentExtent;
    vk.vkCmdSetScissor(cmd, 0, 1, &presentScissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fullscreen.handle);

    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fullscreen.layout,
                               0, 1, &slot.presentSet, 0, nullptr);

    // 3 vertices, no buffer. The shader builds them from gl_VertexIndex.
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

    RecordLayoutTransition(vk, cmd, present.image, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

// Effect: resets the slot's command buffer and records both passes
// Output: false means the buffer is invalid and must not be submitted
//
// Takes the slot but never touches its fence or semaphore -- a rule, not a type.
bool RecordFrame(const VolkDeviceTable& vk,
                 const AcquiredFrame& acquired,
                 const Mesh& mesh, VkDescriptorSet texture,
                 const glm::mat4& camera,
                 const DrawItem* items, uint32_t itemCount) noexcept {
    const FrameSlot& slot = *acquired.slot;
    VkCommandBuffer cmd = slot.cmd;
    // The pool has RESET_COMMAND_BUFFER_BIT, so one buffer can rewind on its own.
    if (vk.vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
        LOG("[vk] vkResetCommandBuffer failed\n");
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;   // recorded once
    if (vk.vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        LOG("[vk] vkBeginCommandBuffer failed\n");
        return false;
    }

    RecordScenePass(vk, slot, mesh, texture, camera, items, itemCount);
    RecordPresentPass(vk, slot, acquired);

    if (vk.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG("[vk] vkEndCommandBuffer failed\n");
        return false;
    }
    return true;
}

int main() {
    // Declarations, in destruction order. The fill order below is different.
    // ========================================================================
    //
    // Creation and destruction cannot be a single line:
    //   create   window/surface before device -- the surface picks the GPU
    //   destroy  the window's swapchain before device -- the device made it
    WindowSystem   windowSystem;   // dies last: glfwTerminate follows every window
    VulkanInstance inst;
    VulkanDevice   dev;
    Window         window;        // holds the swapchain, so it dies before dev
    Commands       commands;
    Descriptors    descriptors;   // slots take sets from this pool
    Pipeline       pipeline;
    Pipeline       fullscreen;
    FrameSlot      slots[kFramesInFlight];   // points at the pipelines, so dies first
    Texture        checker;
    Mesh           mesh;

    // Ask, then build
    // ========================================================================
    //
    //   ask     instance and surface are inputs to the questions, not results
    //   build   device -> commands and descriptors -> what needs them
    //
    // Command buffers divide by when they run, descriptor sets by what they name.

    // glfwInit is first only because windowSystem is declared first and so dies last.
    if (!InitWindowSystem(&windowSystem)) { return 1; }
    if (!CreateInstance(&inst)) { return 1; }
    if (!OpenWindow(inst, 1280, 720, "Lambda Engine", &window)) { return 1; }

    //   formats               our render targets
    //   window.surfaceFormat  the swapchain's; the present pass matches it
    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    RenderTargetFormats formats;
    if (!ChooseRenderTargetFormats(inst, selection.gpu, &formats)) { return 1; }
    if (!SelectSurfaceFormat(inst, selection.gpu, &window)) { return 1; }

    // selection is absorbed into dev here.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }
    if (!CreateCommands(dev, &commands)) { return 1; }

    // Passes
    // ------------------------------------------------------------------------
    //
    // A pass is what stays fixed between BeginRendering and EndRendering; what can
    // change inside belongs to a DrawItem.
    //
    // One pair per pass, here because the set layout comes from the fragment shader
    // and the pipeline from both.
    constexpr const char* kSceneVert = "Shaders/triangle.vert.spv";
    constexpr const char* kSceneFrag = "Shaders/triangle.frag.spv";
    constexpr const char* kPresentVert = "Shaders/fullscreen.vert.spv";
    constexpr const char* kPresentFrag = "Shaders/fullscreen.frag.spv";

    // Different reasons: one scene set per texture, one present set per frame.
    constexpr uint32_t kTextureCount = 1;
    if (!CreateDescriptors(dev, kSceneFrag, kTextureCount,
                           kPresentFrag, kFramesInFlight, &descriptors)) { return 1; }

    // viewportY and cullMode are the pass's, not the shader's. A pipeline and a frame's
    // targets never create each other but must agree on formats -- a pair per pass.
    GraphicsPipelineDesc sceneDesc;
    sceneDesc.vertPath = kSceneVert;
    sceneDesc.fragPath = kSceneFrag;
    sceneDesc.vertexInput = &VertexInput();
    sceneDesc.colorFormat = formats.color;
    sceneDesc.depthFormat = formats.depth;
    sceneDesc.samples = formats.samples;
    sceneDesc.setLayout = descriptors.scene.handle;
    sceneDesc.viewportY = ViewportY::Up;            // our world is y-up
    sceneDesc.cullMode = VK_CULL_MODE_BACK_BIT;
    sceneDesc.polygonMode = VK_POLYGON_MODE_FILL;
    sceneDesc.blending = Blending::Opaque;
    if (!CreateGraphicsPipeline(dev, sceneDesc, &pipeline)) { return 1; }

    // Outlives creation: only colorFormat moves when the surface format changes.
    // No vertex input, no depth, 1 sample -- MSAA ended at the resolve.
    GraphicsPipelineDesc presentDesc;
    presentDesc.vertPath = kPresentVert;
    presentDesc.fragPath = kPresentFrag;
    presentDesc.colorFormat = window.surfaceFormat.format;
    presentDesc.setLayout = descriptors.present.handle;
    presentDesc.viewportY = ViewportY::Down;   // the shader makes its own uv
    presentDesc.cullMode = VK_CULL_MODE_BACK_BIT;
    if (!CreateGraphicsPipeline(dev, presentDesc, &fullscreen)) { return 1; }

    // Render resolution
    // ------------------------------------------------------------------------
    //
    // The other half of what a render target looks like; formats is the first.
    // Constant, so aspect cannot change while the targets live.
    constexpr VkExtent2D kRenderExtent{kRenderWidth, kRenderHeight};
    const float aspect = static_cast<float>(kRenderExtent.width)
                       / static_cast<float>(kRenderExtent.height);

    // No proj[1][1] *= -1: the viewport height is already negative.
    // Depth lands in [0,1] thanks to GLM_FORCE_DEPTH_ZERO_TO_ONE on the CMake target.
    const glm::mat4 proj =
        glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);

    // Frames
    // ------------------------------------------------------------------------
    //
    // Each slot gets the pass it will run and the set for its own resolve image.
    for (FrameSlot& s : slots) {
        if (!CreateFrameSlot(dev, commands, formats, kRenderExtent, &s)) { return 1; }
        s.scene = &pipeline;
        s.present = &fullscreen;
        s.presentSet = AllocateImageSet(descriptors, descriptors.present,
                                        s.targets.resolve.view);
        if (s.presentSet == VK_NULL_HANDLE) { return 1; }
    }

    // Scene
    // ------------------------------------------------------------------------
    //
    // World space; CCW in y-up, so reversing the winding culls the face.
    // Flat in z=0, so all three share one normal (+z) and one tangent (+x, w=1).
    constexpr Vertex vertices[] = {
        {{-0.7f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{-0.7f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.3f,  0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.5f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    };

    constexpr uint16_t kIndices[] = {
        0, 1, 2,          // green triangle
    };

    // Which span is which object. Must stay next to kIndices -- that adjacency is
    // half of the guard.
    constexpr IndexRange kGreenIndices{0, 3};
    static_assert(kGreenIndices.End() == std::size(kIndices),
                  "spans do not cover the index array");

    if (!CreateMesh(dev, commands, vertices, sizeof(vertices),
                    kIndices, static_cast<uint32_t>(std::size(kIndices)), &mesh)) {
        return 1;
    }

    if (!CreateCheckerTexture(dev, commands, &checker)) { return 1; }

    // The set is the (image, sampler) pair, so it belongs to the texture.
    checker.set = AllocateImageSet(descriptors, descriptors.scene, checker.image.view);
    if (checker.set == VK_NULL_HANDLE) { return 1; }

    // No swapchain yet: the loop's EnsureSwapchain makes it, and the first creation
    // takes the same path as a recreation.
    LOG("close the window to exit.\n");

    // Frame state
    // ------------------------------------------------------------------------
    //
    // Everything the loop carries across frames. Not a struct: only lookAt reads the
    // camera values, so grouping would enforce nothing. Split them once something
    // else reads them -- frustum culling, a light's viewpoint, a shadow pass.
    uint32_t slotIndex = 0;       // which slot this frame borrows
    double lastTime = glfwGetTime();

    glm::vec3 eye{0.0f, 0.0f, 2.0f};
    float yaw = -90.0f;           // -90 looks down -z, per the forward expression below
    float pitch = 0.0f;

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // Sleep until an event arrives while minimized.
        //
        // Reset the clock after waking: the sleep is not a frame, and counting it
        // would make the next dt jump and teleport the camera.
        if (!WindowHasDrawableSize(window)) {
            glfwWaitEvents();
            lastTime = glfwGetTime();
            continue;
        }

        // What to draw
        // --------------------------------------------------------------------
        //
        // Nothing here reads the acquire, so it runs before it. Only the camera and
        // the DrawItem array reach the recording layer; the slot holds the rest.

        // One clock reading, two values: t is absolute (object spin), dt is the gap
        // (camera movement). Reading twice would let them drift apart.
        const double now = glfwGetTime();
        const float t = static_cast<float>(now);
        const float dt = static_cast<float>(now - lastTime);
        lastTime = now;

        // Input -> camera state
        // --------------------------------------------------------------------
        //
        // glfwGetKey polls the state glfwPollEvents cached, so this block reads the same
        // value wherever it sits. A callback suits an event; holding a key is a state.
        //
        // Speeds are multiplied by dt, or the frame rate becomes the speed.
        constexpr float kMoveSpeed = 2.0f;    // world units per second
        constexpr float kTurnSpeed = 90.0f;   // degrees per second

        const auto held = [&](int key) {
            return glfwGetKey(window.handle, key) == GLFW_PRESS;
        };

        if (held(GLFW_KEY_LEFT))  { yaw   -= kTurnSpeed * dt; }
        if (held(GLFW_KEY_RIGHT)) { yaw   += kTurnSpeed * dt; }
        if (held(GLFW_KEY_UP))    { pitch += kTurnSpeed * dt; }
        if (held(GLFW_KEY_DOWN))  { pitch -= kTurnSpeed * dt; }

        // At +-90 forward aligns with world up and the cross product below collapses.
        pitch = glm::clamp(pitch, -89.0f, 89.0f);

        const glm::vec3 forward = glm::normalize(glm::vec3{
            glm::cos(glm::radians(yaw)) * glm::cos(glm::radians(pitch)),
            glm::sin(glm::radians(pitch)),
            glm::sin(glm::radians(yaw)) * glm::cos(glm::radians(pitch)),
        });

        // Derived from forward, so it cannot drift out of step with it.
        constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
        const glm::vec3 right = glm::normalize(glm::cross(forward, kWorldUp));

        if (held(GLFW_KEY_W)) { eye += forward * kMoveSpeed * dt; }
        if (held(GLFW_KEY_S)) { eye -= forward * kMoveSpeed * dt; }
        if (held(GLFW_KEY_D)) { eye += right   * kMoveSpeed * dt; }
        if (held(GLFW_KEY_A)) { eye -= right   * kMoveSpeed * dt; }
        if (held(GLFW_KEY_E)) { eye += kWorldUp * kMoveSpeed * dt; }
        if (held(GLFW_KEY_Q)) { eye -= kWorldUp * kMoveSpeed * dt; }

        // center is eye + forward. An absolute target would pin the gaze to one point
        // and rotation would stop working.
        const glm::mat4 view = glm::lookAt(eye, eye + forward, kWorldUp);
        const glm::mat4 camera = proj * view;

        // One item, so nothing here orders anything yet. Order becomes a question at
        // two items, and a measurable one further out.
        const glm::vec3 kZAxis{0.0f, 0.0f, 1.0f};
        const DrawItem items[] = {
            // green, spinning about z so its depth does not change
            {glm::rotate(glm::mat4(1.0f), t, kZAxis), 1.0f, kGreenIndices},
        };

        // Draw it
        // --------------------------------------------------------------------

        const FrameSlot& slot = slots[slotIndex];

        AcquiredFrame acquired;
        const FrameResult begun = BeginFrame(dev, &window, slot, &acquired);
        if (begun == FrameResult::Fatal) { break; }

        if (begun == FrameResult::Skip) { continue; }

        // Rebuild the fullscreen pipeline if the surface format changed.
        //
        // BeginFrame raises the flag, so checking at the top of the loop would be one
        // iteration late and this frame would draw with the stale pipeline. Scene
        // pipelines are untouched: their format is our render target's, not the surface's.
        //
        // The wait satisfies DestroyPipeline's contract (Pipeline.h). BeginFrame waited
        // on this frame's fence only, which is not enough.
        if (window.surfaceFormatChanged) {
            dev.table.vkDeviceWaitIdle(dev.handle);
            DestroyPipeline(dev, &fullscreen);
            presentDesc.colorFormat = window.surfaceFormat.format;   // the only field that moved
            if (!CreateGraphicsPipeline(dev, presentDesc, &fullscreen)) {
                break;
            }
            window.surfaceFormatChanged = false;   // cleared by whoever handled it
        }

        // These break instead of continue. After the acquire, skipping the submit leaves
        // a signalled semaphore and a reset fence with nobody to wait on them.
        if (!RecordFrame(dev.table, acquired, mesh, checker.set, camera,
                         items, static_cast<uint32_t>(std::size(items)))) {
            break;
        }

        // Frame.h holds the reason submit and present are separate.
        if (!SubmitFrame(dev, acquired)) {
            break;
        }
        if (!PresentFrame(dev, &window, *acquired.image)) {
            break;
        }

        slotIndex = (slotIndex + 1) % kFramesInFlight;
    }

    // Destructors run in reverse declaration order. This wait stays because
    // ~VulkanDevice waits only after every other destructor has already run, and the
    // swapchain and command pool may still be in use by the GPU.
    dev.table.vkDeviceWaitIdle(dev.handle);

    LOG("[vk] clean shutdown\n");
    return 0;
}
