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

#include <cmath>      // cos, sin
#include <cstring>    // memcpy
#include <iterator>   // std::size

// One header at a time. <glm/ext.hpp> was dropped when vendoring (VERSION.md).
#include <glm/common.hpp>                  // clamp
#include <glm/ext/matrix_clip_space.hpp>   // perspective
#include <glm/ext/matrix_transform.hpp>    // rotate, translate, scale, lookAt
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
// One pass, two stages: a stage is one pipeline and its own BeginRendering scope, and
// the draw target is fixed inside it. The second stage reads what the first wrote:
//
//   [opaque stage]   knows nothing about the window
//     barrier x3 (the pass's color, colorResolve, depth for this slot)
//     BeginRendering   attachment = color / depth, resolving into colorResolve
//       BindVertexBuffers, BindIndexBuffer
//       BindPipeline, BindDescriptorSets
//       per item: PushConstants, DrawIndexed
//     EndRendering    <- the multisample average happens here
//
//   [present stage] reads the resolve image only. A post effect goes here
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

// What differs between draws, once the stage has fixed everything else.
//
// No pipeline, texture or camera: the stage holds one of each. Each moves in here the
// day one stage needs two of it, and the bind then moves into the loop with it.
struct DrawItem {
    glm::mat4 model{1.0f};
    float alpha = 1.0f;
    IndexRange range{};
};

// Opaque stage
//
// Input:  the pass (attachments, mesh, texture) and the slot (command buffer, items)
// Effect: appends commands that draw into this slot's color / depth
//
// No swapchain, so this works without a window. No camera either: it went into the
// slot's uniform, which every draw in this stage reads.
//
// One mesh for every item: the spans in items index into it. A second mesh means
// another BindVertexBuffers, which is why the bind sits above the loop and not in it.
static void RecordOpaqueStage(const FrameSlot& slot, const ScenePass& scene,
                              const Pipeline& pipeline) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    const DrawItem* items = slot.items;
    const uint32_t itemCount = slot.itemCount;
    VkCommandBuffer cmd = slot.cmd;
    const Mesh& mesh = *scene.mesh;

    // This slot's frame of the pass. index picks the descriptor sets too, so the
    // attachments and the sets that name them cannot come apart.
    const ScenePass::PerFrame& targets = scene.frames[slot.index];
    const VkExtent2D extent = targets.color.desc.extent;   // render resolution, not window size

    // oldLayout UNDEFINED: loadOp=CLEAR overwrites, so the old contents are dead.
    // Asking to preserve them makes the driver actually copy.
    RecordLayoutTransition(vk, cmd, targets.color.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // The resolve target is written too, at the end of the pass, so it needs the same
    // layout and the same stage. Nothing here draws into it directly.
    RecordLayoutTransition(vk, cmd, targets.colorResolve.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Depth test runs at EARLY/LATE_FRAGMENT_TESTS, ahead of COLOR_ATTACHMENT_OUTPUT.
    // Reusing the color stage here would let depth writes pass the barrier.
    RecordLayoutTransition(vk, cmd, targets.depth.image.handle, VK_IMAGE_ASPECT_DEPTH_BIT,
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
    color.imageView = targets.color.image.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = targets.colorResolve.image.view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.clearValue.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    // Clear 1.0 = farthest, paired with the pipeline's compareOp=LESS.
    // DONT_CARE: depth is used only within this frame.
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = targets.depth.image.view;
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
    const VkViewport viewport = MakeViewport(extent, pipeline.desc.viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);

    // Pixels outside this rect are discarded. Whole screen for now.
    VkRect2D scissor{};
    scissor.extent = extent;
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
                               0, 1, &slot.descriptors->sceneSets[slot.index], 0, nullptr);

    // binding 0 matches the pipeline's binding 0. offset changes once several meshes
    // share one buffer.
    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.handle, &offset);

    // Index buffers have no slot number: a command buffer holds exactly one.
    // Contract: this type must match the element type of kIndices.
    vk.vkCmdBindIndexBuffer(cmd, mesh.indices.handle, 0, mesh.desc.indexType);

    // Order is whatever the caller wrote into the array. This layer does not sort.
    // Nothing is bound in here, so the order only decides blending.
    for (uint32_t i = 0; i < itemCount; ++i) {
        const DrawItem& item = items[i];

        // viewProj is in the uniform this set already points at; only the item's own
        // values ride the command buffer.
        const PushConstants push{item.model, item.alpha};
        vk.vkCmdPushConstants(cmd, pipeline.layout,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(push), &push);

        // firstIndex is a position in the index buffer. vertexOffset (0) is added to
        // every index, which matters once meshes number their vertices from zero.
        vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex, 0, 0);
    }

    vk.vkCmdEndRendering(cmd);
}

// Present stage
//
// Input:  the slot's resolve texture, and where to put it
// Effect: appends commands that sample the resolve image into the swapchain image
static void RecordPresentStage(const FrameSlot& slot, const ScenePass& scene,
                               const Pipeline& pipeline) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;

    // What the scene pass left behind. The set bound below names this same image,
    // and both are picked by slot.index.
    const Texture& source = scene.frames[slot.index].colorResolve;
    const Texture& dest = slot.image->texture;
    const VkExtent2D destExtent = dest.desc.extent;

    // Written as an attachment, read as a texture -- that is this whole pass. The
    // layout must equal the one recorded into the descriptor set.
    RecordLayoutTransition(vk, cmd, source.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // srcStage must overlap SubmitFrame's wait stage, or this transition can run ahead
    // of the acquire.
    RecordLayoutTransition(vk, cmd, dest.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Window sized, unlike the opaque stage. The sampler's LINEAR filter scales.
    VkRenderingAttachmentInfo swapColor{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    swapColor.imageView = dest.image.view;
    swapColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // the draw covers everything
    swapColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = destExtent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &swapColor;

    vk.vkCmdBeginRendering(cmd, &rendering);

    // Opposite sign from the opaque stage: this pipeline is built ViewportY::Down.
    const VkViewport viewport = MakeViewport(destExtent, pipeline.desc.viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = destExtent;
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);

    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
                               0, 1, &slot.descriptors->presentSets[slot.index], 0, nullptr);

    // 3 vertices, no buffer. The shader builds them from gl_VertexIndex.
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

    RecordLayoutTransition(vk, cmd, dest.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

// Effect: resets the slot's command buffer and records both stages from it
// Output: false means the buffer is invalid and must not be submitted
//
// Takes the slot but never touches its fence or semaphore -- a rule, not a type.
bool RecordFrame(const FrameSlot& slot, const ScenePass& scene,
                 const Pipeline& opaque, const Pipeline& present) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;

    // The value and its GPU copy meet here. Safe because BeginFrame waited on this
    // slot's fence, and this runs after it -- an acquired image is its precondition.
    std::memcpy(slot.uniform.mapped, &slot.scene, sizeof(slot.scene));
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

    RecordOpaqueStage(slot, scene, opaque);
    RecordPresentStage(slot, scene, present);

    if (vk.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG("[vk] vkEndCommandBuffer failed\n");
        return false;
    }
    return true;
}

// Scene data made in code
// ============================================================================
//
// Neither is about Vulkan: both hand back plain arrays, which is what CreateMesh and
// CreateTextureFromPixels take. A file loader fills the same arrays.

// A UV sphere. On a unit sphere the position is also the normal, which is the whole
// reason this shape shows lighting.
//
// Output: vertices[(stacks+1) * (slices+1)], indices[stacks * slices * 6]
static void MakeSphere(uint32_t stacks, uint32_t slices, float radius,
                       Vertex* vertices, uint16_t* indices) noexcept {
    for (uint32_t stack = 0; stack <= stacks; ++stack) {
        // phi from the +y pole down to -y; theta all the way around.
        const float phi = 3.14159265f * static_cast<float>(stack) / stacks;
        for (uint32_t slice = 0; slice <= slices; ++slice) {
            const float theta = 6.28318531f * static_cast<float>(slice) / slices;
            const float sinPhi = std::sin(phi);
            const float nx = sinPhi * std::cos(theta);
            const float ny = std::cos(phi);
            const float nz = sinPhi * std::sin(theta);

            Vertex& v = vertices[stack * (slices + 1) + slice];
            v.position[0] = nx * radius;
            v.position[1] = ny * radius;
            v.position[2] = nz * radius;
            v.normal[0] = nx; v.normal[1] = ny; v.normal[2] = nz;
            v.uv[0] = static_cast<float>(slice) / slices;
            v.uv[1] = static_cast<float>(stack) / stacks;

            // Along increasing theta -- the direction u grows in, which is what a
            // normal map will expect.
            v.tangent[0] = -std::sin(theta);
            v.tangent[1] = 0.0f;
            v.tangent[2] =  std::cos(theta);
            v.tangent[3] = 1.0f;
        }
    }

    // Two triangles per quad. CCW seen from outside, which is what BACK culling wants.
    uint32_t written = 0;
    for (uint32_t stack = 0; stack < stacks; ++stack) {
        for (uint32_t slice = 0; slice < slices; ++slice) {
            const uint16_t top = static_cast<uint16_t>(stack * (slices + 1) + slice);
            const uint16_t bottom = static_cast<uint16_t>(top + slices + 1);
            indices[written++] = top;
            indices[written++] = bottom;
            indices[written++] = static_cast<uint16_t>(top + 1);
            indices[written++] = static_cast<uint16_t>(top + 1);
            indices[written++] = bottom;
            indices[written++] = static_cast<uint16_t>(bottom + 1);
        }
    }
}

// Small cells on purpose: one texel per cell makes a wrong uv obvious, and the
// LINEAR sampler softens the edges.
//
// Output: pixels[size * size * 4], RGBA8
static void MakeChecker(uint32_t size, uint8_t* pixels) noexcept {
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint8_t v = ((x + y) % 2 == 0) ? 255 : 70;
            uint8_t* p = pixels + (y * size + x) * 4;
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    }
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
    Pipeline       opaque;        // each owns the set layout its shaders declare
    Pipeline       present;
    Descriptors    descriptors;   // borrows those layouts, so it dies before them
    ScenePass      scene;         // the attachments every frame draws into
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
    if (!OpenWindow(inst, kWindowWidth, kWindowHeight, "Lambda Engine", &window)) {
        return 1;
    }

    // The only question asked of the hardware. It answers with the GPU, its queues,
    // and what we can draw into on it -- a GPU that cannot do the last one is not a
    // candidate. The second stage's format is the swapchain's, and that is made in
    // the loop, so nothing asks for it here.
    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }
    const AttachmentFormats formats = selection.formats;

    // The queue side of selection is absorbed into dev here; formats outlive it.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }
    if (!CreateCommands(dev, &commands)) { return 1; }

    // Passes
    // ------------------------------------------------------------------------
    //
    // One pipeline per stage. What stays fixed inside a stage lives here; what can
    // change between draws belongs to a DrawItem.
    //
    // viewportY and cullMode are the pass's, not the shader's. A pipeline and a frame's
    // targets never create each other but must agree on formats -- a pair per pass.
    GraphicsPipelineDesc opaqueDesc;
    opaqueDesc.vertPath = "Shaders/mesh.vert.spv";
    opaqueDesc.fragPath = "Shaders/mesh.frag.spv";
    opaqueDesc.vertexInput = &VertexInput();
    opaqueDesc.formats = formats;
    opaqueDesc.viewportY = ViewportY::Up;            // our world is y-up
    opaqueDesc.cullMode = VK_CULL_MODE_BACK_BIT;
    opaqueDesc.polygonMode = VK_POLYGON_MODE_FILL;
    opaqueDesc.blending = Blending::Opaque;
    if (!CreateGraphicsPipeline(dev, opaqueDesc, &opaque)) { return 1; }

    // Built with an empty format: the swapchain decides that, and the swapchain is
    // made in the loop, which rebuilds this the first time it sees one.
    // No vertex input, no depth, 1 sample -- MSAA ended at the resolve.
    GraphicsPipelineDesc presentDesc;
    presentDesc.vertPath = "Shaders/fullscreen.vert.spv";
    presentDesc.fragPath = "Shaders/fullscreen.frag.spv";
    presentDesc.viewportY = ViewportY::Down;   // the shader makes its own uv
    presentDesc.cullMode = VK_CULL_MODE_BACK_BIT;
    if (!CreateGraphicsPipeline(dev, presentDesc, &present)) { return 1; }

    // After the pipelines: the layouts are theirs, read out of the same .spv the
    // stages were compiled from. Both counts are per frame in flight -- the scene set
    // holds that frame's uniform, so it cannot be shared any more than the uniform can.
    if (!CreateDescriptors(dev, opaque.setLayout, kFramesInFlight,
                           present.setLayout, kFramesInFlight,
                           &descriptors)) { return 1; }

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
        glm::perspective(glm::radians(kFovDegrees), aspect, kNearPlane, kFarPlane);

    // Scene
    // ------------------------------------------------------------------------
    //
    // Test data, made in code: no loader yet, and what we are checking is the path
    // from bytes to GPU, not a file format. A loader replaces the two Make* calls.
    constexpr uint32_t kStacks = 16;
    constexpr uint32_t kSlices = 32;
    constexpr float kRadius = 0.7f;
    constexpr uint32_t kVertexCount = (kStacks + 1) * (kSlices + 1);
    constexpr uint32_t kIndexCount = kStacks * kSlices * 6;
    static_assert(kVertexCount <= 0xFFFF, "index type is uint16");

    Vertex vertices[kVertexCount]{};
    uint16_t kIndices[kIndexCount]{};
    MakeSphere(kStacks, kSlices, kRadius, vertices, kIndices);

    constexpr IndexRange kSphereIndices{0, kIndexCount};
    static_assert(kSphereIndices.End() == kIndexCount,
                  "spans do not cover the index array");

    // stride is the one thing a mesh can say about its vertices; the pipeline says
    // which bytes are what.
    const MeshDesc meshDesc{sizeof(Vertex), kVertexCount, kIndexCount};
    if (!CreateMesh(dev, commands, meshDesc, vertices, kIndices, &mesh)) { return 1; }

    constexpr uint32_t kCheckerSize = 8;
    uint8_t checkerPixels[kCheckerSize * kCheckerSize * 4]{};
    MakeChecker(kCheckerSize, checkerPixels);

    // SRGB: this is multiplied with the shader's output, so it must be in the same
    // space as the render target. UNORM here would brighten the result.
    const TextureDesc checkerDesc{{kCheckerSize, kCheckerSize},
                                  VK_FORMAT_R8G8B8A8_SRGB, VK_SAMPLE_COUNT_1_BIT,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                      | VK_IMAGE_USAGE_SAMPLED_BIT};
    if (!CreateTextureFromPixels(dev, commands, checkerDesc, checkerPixels,
                                 sizeof(checkerPixels), &checker)) { return 1; }

    // Frames
    // ------------------------------------------------------------------------
    //
    // The pass owns the attachments; a slot owns the command buffer and the sets that
    // name them. So the pass is built first, and every slot reads its own frame of it.
    if (!CreateScenePass(dev, formats, kRenderExtent, mesh, checker, &scene)) { return 1; }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateFrameSlot(dev, commands, descriptors, i, scene, &slots[i])) { return 1; }
    }

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

    glm::vec3 eye{0.0f, 0.0f, 3.5f};
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
        // Nothing here touches the GPU, so it could run while minimized. What comes
        // out is state -- camera, light, items -- and the next section sends it.

        // Clock
        //
        // One clock reading, two values: t is absolute (object spin), dt is the gap
        // (camera movement). Reading twice would let them drift apart.
        const double now = glfwGetTime();
        const float t = static_cast<float>(now);
        const float dt = static_cast<float>(now - lastTime);
        lastTime = now;

        // Input -> camera
        //
        // --------------------------------------------------------------------
        //
        // glfwGetKey polls the state glfwPollEvents cached, so this block reads the same
        // value wherever it sits. A callback suits an event; holding a key is a state.
        //
        // Speeds are multiplied by dt, or the frame rate becomes the speed.

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

        // Light
        //
        // One directional light, circling so the brightness visibly changes -- the
        // objects turn about z, which leaves their normals fixed.
        const glm::vec3 lightDir = glm::normalize(
            glm::vec3{std::cos(t) * 0.7f, 0.5f, std::sin(t) * 0.7f});

        // Items
        //
        // Five items and nothing here sorts them. With one pipeline and one texture
        // the order costs nothing yet; it starts to matter when either becomes two.
        const glm::vec3 kZAxis{0.0f, 0.0f, 1.0f};
        const glm::vec3 kYAxis{0.0f, 1.0f, 0.0f};

        // One mesh, five items: only the matrix differs, so the whole cost of another
        // object is one DrawItem. The spans are identical because they all index the
        // same sphere.
        const glm::mat4 half = glm::scale(glm::mat4(1.0f), glm::vec3{0.5f});
        const DrawItem items[] = {
            // Centre, spinning about z: the sphere looks the same, but the checker
            // slides over it, so the texture and the lighting are visibly separate.
            {glm::rotate(glm::mat4(1.0f), t, kZAxis), 1.0f, kSphereIndices},

            // Left and right, turning the other way and about y.
            {glm::translate(glm::mat4(1.0f), glm::vec3{-1.5f, 0.0f, 0.0f})
                 * glm::rotate(glm::mat4(1.0f), -t, kYAxis) * half,
             1.0f, kSphereIndices},
            {glm::translate(glm::mat4(1.0f), glm::vec3{1.5f, 0.0f, 0.0f})
                 * glm::rotate(glm::mat4(1.0f), t * 1.7f, kYAxis) * half,
             1.0f, kSphereIndices},

            // Behind and in front, so depth has something to sort out.
            {glm::translate(glm::mat4(1.0f), glm::vec3{0.0f, 1.1f, -1.2f}) * half,
             1.0f, kSphereIndices},
            {glm::translate(glm::mat4(1.0f), glm::vec3{0.0f, -1.0f, 0.9f}) * half,
             1.0f, kSphereIndices},
        };

        // Fill the slot
        //
        // Assignment only, so it belongs up here: what reaches the GPU, and when, is
        // RecordFrame's. slot is this frame's, and these three are what changes in it.
        FrameSlot& slot = slots[slotIndex];
        slot.scene = {camera, glm::vec4{lightDir, 0.0f},
                      glm::vec4{1.0f, 0.95f, 0.9f, 0.15f}, glm::vec4{eye, 48.0f}};
        slot.items = items;
        slot.itemCount = static_cast<uint32_t>(std::size(items));

        // Draw it
        // --------------------------------------------------------------------

        const FrameResult begun = BeginFrame(dev, &window, &slot);
        if (begun == FrameResult::Fatal) { break; }

        if (begun == FrameResult::Skip) { continue; }

        // Both sides say what format they are, so the mismatch is the whole test -- no
        // flag to raise and no flag to forget to clear. True on the first frame, and
        // again whenever the window moves to a monitor with a different surface format.
        //
        // Right after BeginFrame, not at the top: the swapchain is remade in there, and
        // one iteration later this frame would draw with the stale pipeline.
        const VkFormat target = slot.image->texture.desc.format;
        if (present.desc.formats.color != target
            && !RebuildPipeline(dev, AttachmentFormats{target}, &present)) {
            break;
        }

        // These break instead of continue. After the acquire, skipping the submit
        // leaves a signalled semaphore and a reset fence with nobody to wait on them.
        if (!RecordFrame(slot, scene, opaque, present)) {
            break;
        }

        // Frame.h holds the reason submit and present are separate.
        if (!SubmitFrame(dev, slot)) {
            break;
        }
        if (!PresentFrame(dev, &window, slot)) {
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
