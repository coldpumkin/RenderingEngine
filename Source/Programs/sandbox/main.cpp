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
//       per item: BindPipeline and BindDescriptorSets only when they change,
//                 PushConstants, DrawIndexed
//     EndRendering    <- the multisample average happens here
//
//   [present pass]  reads draw.resolve only
//     barrier draw.resolve -> SHADER_READ_ONLY
//     barrier swapchain  -> COLOR_ATTACHMENT
//     BeginRendering   attachment = swapchain image, in the swapchain's own format
//       BindPipeline, BindDescriptorSets (draw.resolveSet), Draw 3 vertices
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

// Everything one draw needs.
//
// Not an object: no transform, no mesh, no material. Only what actually differs
// between draws.
//
// pipeline and texture are handles because several items share one and the loop binds
// only on change. They are separate fields because they change at different items --
// five objects give three pipeline binds and four texture binds. Two axes.
struct DrawItem {
    const Pipeline* pipeline = nullptr;
    VkDescriptorSet texture = VK_NULL_HANDLE;
    PushConstants push{};        // mvp + alpha, both per object
    IndexRange range{};
};

// Scene pass
//
// Input:  cmd, draw, vertex/index buffer, items
// Effect: appends commands that draw into draw.color / draw.depth
//
// No swapchain, so this works without a window. No camera and no time either: those
// are scene state, not recording. All this knows is "draw this index span with this
// pipeline and this texture, using this matrix".
static void RecordScenePass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                            const RenderTargets& draw,
                            const Buffer& vertexBuffer,
                            const Buffer& indexBuffer,
                            const DrawItem* items, uint32_t itemCount) noexcept {
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

    // Nothing to draw: clear and leave. The viewport below reads items[0].
    if (itemCount == 0) {
        vk.vkCmdEndRendering(cmd);
        return;
    }

    // Dynamic state, so a resize does not rebuild the pipeline.
    //
    // The sign comes from the pipeline itself, so it cannot disagree with the frontFace
    // baked into it (Pipeline.h). Reading items[0] assumes every scene pipeline shares
    // one y convention; mixing conventions moves these two lines into the loop.
    const VkViewport viewport = MakeViewport(extent, items[0].pipeline->viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);

    // Pixels outside this rect are discarded. Whole screen for now.
    VkRect2D scissor{};
    scissor.extent = extent;
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    // binding 0 matches the pipeline's binding 0. offset changes once several meshes
    // share one buffer.
    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.handle, &offset);

    // Index buffers have no slot number: a command buffer holds exactly one.
    // Contract: this type must match the element type of kIndices.
    vk.vkCmdBindIndexBuffer(cmd, indexBuffer.handle, 0, VK_INDEX_TYPE_UINT16);

    // Order is whatever the caller wrote into the array, including the rule that
    // translucent goes last.
    //
    // Two bind conditions stand side by side because they change at different items.
    const Pipeline* boundPipeline = nullptr;
    VkDescriptorSet boundTexture = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < itemCount; ++i) {
        const DrawItem& item = items[i];

        if (item.pipeline != boundPipeline) {
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 item.pipeline->handle);
            boundPipeline = item.pipeline;
        }

        // A pipeline change does not unbind the set: the scene pipelines share one
        // layout definition (same push range, same set layout), so they are compatible.
        // That is what lets this condition stand apart from the one above. Mixing in a
        // pipeline with a different layout breaks it.
        if (item.texture != boundTexture) {
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       item.pipeline->layout, 0, 1, &item.texture,
                                       0, nullptr);
            boundTexture = item.texture;
        }

        vk.vkCmdPushConstants(cmd, item.pipeline->layout,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(item.push), &item.push);

        // firstIndex is a position in the index buffer. vertexOffset (0) is added to
        // every index, which matters once meshes number their vertices from zero.
        vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex, 0, 0);
    }

    vk.vkCmdEndRendering(cmd);
}

// Present pass
//
// Input:  cmd, draw (read only), present, presentExtent, fullscreen
// Effect: appends commands that sample draw.resolve into the swapchain image
//
// Taking draw is what "reads the previous pass" means: resolveSet comes from the same
// draw, so the pair cannot disagree.
//
// draw.color is never touched here. It is the multisample image, which our shader
// cannot sample; the scene pass already averaged it into draw.resolve.
static void RecordPresentPass(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                              const RenderTargets& draw,
                              const SwapchainImage& present,
                              VkExtent2D presentExtent,
                              const Pipeline& fullscreen) noexcept {
    // Writing must finish (COLOR_ATTACHMENT_OUTPUT) before sampling (FRAGMENT_SHADER).
    // The layout must equal the one recorded into the descriptor set.
    //
    // The write being waited on is the resolve, which counts as a colour attachment
    // write in the same stage, so the barrier did not change when MSAA arrived.
    RecordLayoutTransition(vk, cmd, draw.resolve.handle, VK_IMAGE_ASPECT_COLOR_BIT,
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

    // Differs from the scene pass: swapchain attachment, no depth, no vertex buffer,
    // window sized. The sampler's LINEAR filter scales between the two sizes.
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

    // Opposite sign from the scene pass, because the fullscreen pipeline is built with
    // ViewportY::Down. The reason lives there.
    const VkViewport presentViewport = MakeViewport(presentExtent, fullscreen.viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &presentViewport);

    VkRect2D presentScissor{};
    presentScissor.extent = presentExtent;
    vk.vkCmdSetScissor(cmd, 0, 1, &presentScissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fullscreen.handle);

    // An image cannot ride in the command stream the way a push constant does, so the
    // command only says "attach this set at slot 0".
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fullscreen.layout,
                               0, 1, &draw.resolveSet, 0, nullptr);

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

// Input:  cmd, target, vertex/index buffer, items, fullscreen
// Effect: resets cmd and records both passes
// Output: false means cmd is invalid and must not be submitted
bool RecordFrame(const VolkDeviceTable& vk,
                 VkCommandBuffer cmd,
                 const FrameTarget& target,
                 const Buffer& vertexBuffer,
                 const Buffer& indexBuffer,
                 const DrawItem* items, uint32_t itemCount,
                 const Pipeline& fullscreen) noexcept {
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
    Descriptors    descriptors;   // frames take sets from this pool
    Frame          frames[kFramesInFlight];
    Pipeline       pipeline;
    Pipeline       wireframe;     // same vertices as lines
    Pipeline       translucent;   // blend on, depth write off
    Pipeline       fullscreen;
    Texture        checker;
    Texture        stripe;
    Buffer         vertexBuffer;
    Buffer         indexBuffer;   // dies first

    // Fill, in dependency order. An early return leaks nothing.
    // ========================================================================

    // glfwInit is first only because windowSystem is declared first and so dies last.
    if (!InitWindowSystem(&windowSystem)) { return 1; }
    if (!CreateInstance(&inst)) { return 1; }
    if (!OpenWindow(inst, 1280, 720, "Lambda Engine", &window)) { return 1; }

    // Pick the GPU and ask it everything at once. Two formats have to be kept in step:
    //   formats               what our render targets use
    //   window.surfaceFormat  what the swapchain uses; the present pass matches it
    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    const RenderTargetFormats formats = ChooseRenderTargetFormats(inst, selection.gpu);
    if (formats.depth == VK_FORMAT_UNDEFINED) {
        LOG("[vk] no usable depth format\n");
        return 1;
    }
    if (!SelectSurfaceFormat(inst, selection.gpu, &window)) { return 1; }

    // selection is absorbed into dev here.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }
    if (!CreateCommands(dev, &commands)) { return 1; }

    // Two set counts, counted from different things: one scene set per texture, one
    // present set per frame. The pool never grows, so a missed increment fails
    // allocation later.
    //
    // Contract: kTextureCount must equal the number of Texture declarations above.
    // Deriving it needs the textures in an array, which would turn checker/stripe into
    // indices -- worth it only once textures are chosen by id rather than by name.
    constexpr uint32_t kTextureCount = 2;
    if (!CreateDescriptors(dev, kTextureCount, kFramesInFlight, &descriptors)) { return 1; }

    for (Frame& f : frames) {
        if (!CreateFrame(dev, commands, descriptors, formats, &f)) { return 1; }
    }

    // Two things differ per pass, and they differ for different reasons: the format is
    // what we draw into, the set layout is what the shader reads.
    if (!CreateTrianglePipeline(dev, formats, descriptors.sceneLayout,
                                VK_POLYGON_MODE_FILL, Blending::Opaque, &pipeline)) { return 1; }
    if (!CreateTrianglePipeline(dev, formats, descriptors.sceneLayout,
                                VK_POLYGON_MODE_LINE, Blending::Opaque, &wireframe)) { return 1; }
    if (!CreateTrianglePipeline(dev, formats, descriptors.sceneLayout,
                                VK_POLYGON_MODE_FILL, Blending::Translucent, &translucent)) { return 1; }
    if (!CreateFullscreenPipeline(dev, window.surfaceFormat.format,
                                  descriptors.presentLayout, &fullscreen)) {
        return 1;
    }

    // Two triangles overlap and are drawn in the opposite order to their depth, so the
    // overlap shows whether the depth test runs: green means on, red means off.
    //
    // Coordinates are world space, so z is distance from the camera and equally sized
    // triangles shrink with distance.
    // uv runs y-down: world is y-up, so the top vertex is v=0.
    constexpr Vertex kTriangles[] = {
        // near (z=0, 2 from the camera), drawn first -- green
        {{-0.7f,  0.5f,  0.0f}, {0.1f, 0.9f, 0.2f}, {0.0f, 0.0f}},
        {{-0.7f, -0.5f,  0.0f}, {0.1f, 0.9f, 0.2f}, {0.0f, 1.0f}},
        {{ 0.3f,  0.0f,  0.0f}, {0.1f, 0.9f, 0.2f}, {1.0f, 0.5f}},

        // far (z=-1.5, 3.5 from the camera), drawn second -- red
        {{ 0.7f,  0.5f, -1.5f}, {0.9f, 0.2f, 0.1f}, {1.0f, 0.0f}},
        {{-0.3f,  0.0f, -1.5f}, {0.9f, 0.2f, 0.1f}, {0.0f, 0.5f}},
        {{ 0.7f, -0.5f, -1.5f}, {0.9f, 0.2f, 0.1f}, {1.0f, 1.0f}},

        // nearest (z=0.5, 1.5 from the camera) -- translucent blue
        {{-0.2f,  0.6f,  0.5f}, {0.2f, 0.3f, 0.95f}, {0.0f, 0.0f}},
        {{-0.2f, -0.4f,  0.5f}, {0.2f, 0.3f, 0.95f}, {0.0f, 1.0f}},
        {{ 0.8f,  0.1f,  0.5f}, {0.2f, 0.3f, 0.95f}, {1.0f, 0.5f}},
    };

    // Four vertices, two triangles: two of them get indexed twice.
    // Same winding as above (CCW in y-up); reversing it gets the face culled.
    constexpr Vertex kQuad[] = {
        {{-1.4f,  0.45f, -0.8f}, {0.95f, 0.75f, 0.15f}, {0.0f, 0.0f}},
        {{-1.4f, -0.45f, -0.8f}, {0.95f, 0.75f, 0.15f}, {0.0f, 1.0f}},
        {{-0.5f, -0.45f, -0.8f}, {0.95f, 0.75f, 0.15f}, {1.0f, 1.0f}},
        {{-0.5f,  0.45f, -0.8f}, {0.95f, 0.75f, 0.15f}, {1.0f, 0.0f}},
    };

    Vertex vertices[std::size(kTriangles) + std::size(kQuad)]{};
    std::memcpy(vertices, kTriangles, sizeof(kTriangles));
    std::memcpy(vertices + std::size(kTriangles), kQuad, sizeof(kQuad));

    // kQuadBase is where the memcpy above placed the quad. Deriving it means adding a
    // triangle cannot silently point the quad at the wrong vertices.
    //
    // Contract: the element type must match VK_INDEX_TYPE_UINT16 at the bind site.
    // A mismatch compiles, runs, and draws the wrong vertices.
    constexpr uint16_t kQuadBase = static_cast<uint16_t>(std::size(kTriangles));
    constexpr uint16_t kIndices[] = {
        0, 1, 2,          // green triangle
        3, 4, 5,          // red triangle
        6, 7, 8,          // blue triangle (translucent)
        kQuadBase + 0, kQuadBase + 1, kQuadBase + 2,   // quad, first half
        kQuadBase + 2, kQuadBase + 3, kQuadBase + 0,   // quad, second half
    };

    // Which span belongs to which object. Declared in the same order as kIndices, each
    // one starting where the previous ended, so inserting in the middle shifts the rest.
    // These must stay next to kIndices: that adjacency is half of the guard.
    constexpr IndexRange kGreenIndices{0, 3};
    constexpr IndexRange kRedIndices{kGreenIndices.End(), 3};
    constexpr IndexRange kBlueIndices{kRedIndices.End(), 3};
    constexpr IndexRange kQuadIndices{kBlueIndices.End(), 6};
    static_assert(kQuadIndices.End() == std::size(kIndices),
                  "spans do not cover the index array");

    if (!CreateDeviceLocalBuffer(dev, commands, vertices, sizeof(vertices),
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertexBuffer)) {
        return 1;
    }
    if (!CreateDeviceLocalBuffer(dev, commands, kIndices, sizeof(kIndices),
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indexBuffer)) {
        return 1;
    }

    if (!CreateCheckerTexture(dev, commands, &checker)) { return 1; }
    if (!CreateStripeTexture(dev, commands, &stripe)) { return 1; }

    // A set now names two images, so it is a pair rather than a property of one texture.
    // Both textures have to exist before either set can be filled, which is why this sits
    // here instead of inside Create*Texture.
    checker.set = AllocateSceneSet(descriptors, checker.image.view, stripe.image.view);
    stripe.set = AllocateSceneSet(descriptors, stripe.image.view, checker.image.view);
    if (checker.set == VK_NULL_HANDLE || stripe.set == VK_NULL_HANDLE) { return 1; }

    // No swapchain here. The loop's EnsureSwapchain creates it, and the first creation
    // takes the same path as a recreation: "nothing to draw into" is a normal state.
    LOG("close the window to exit.\n");

    // Projection: its period is the render target's lifetime, not the frame.
    // ========================================================================
    //
    // The render target extent is a compile-time constant, so aspect cannot change
    // while the targets live -- a window resize does not touch it. It would change only
    // if the render resolution became a runtime value, and that comes with a render
    // target rebuild path for these lines to follow.
    //
    // Read from frames[0] because every frame is built at the same size.
    const VkExtent2D sceneExtent = frames[0].targets.extent;
    const float aspect = static_cast<float>(sceneExtent.width)
                       / static_cast<float>(sceneExtent.height);

    // No proj[1][1] *= -1: the viewport height is already negative.
    // Depth lands in [0,1] thanks to GLM_FORCE_DEPTH_ZERO_TO_ONE on the CMake target.
    const glm::mat4 proj =
        glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);

    // State that crosses frames. This is all of it.
    // ========================================================================
    //
    // Not a struct: the values sit together and only lookAt reads them, so grouping
    // would enforce nothing. Split it when a second consumer appears -- frustum
    // culling, a light's viewpoint, a shadow pass.
    uint32_t frameIndex = 0;      // which frame's resources are up
    double lastTime = glfwGetTime();

    glm::vec3 eye{0.0f, 0.0f, 2.0f};
    float yaw = -90.0f;           // -90 looks down -z, per the forward expression below
    float pitch = 0.0f;

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // Sleep until an event arrives while minimized.
        //
        // Reset the clock after waking: the sleep is not a frame, and counting it would
        // make the next dt jump and teleport the camera. The Skip below is the opposite.
        if (!WindowHasDrawableSize(window)) {
            glfwWaitEvents();
            lastTime = glfwGetTime();
            continue;
        }

        const Frame& frame = frames[frameIndex];

        FrameTarget target;
        const FrameResult begun = BeginFrame(dev, &window, frame, &target);
        if (begun == FrameResult::Fatal) { break; }

        // The clock is untouched here. A Skip is short and that time really passed;
        // dropping it would stall the camera during a resize drag.
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
            if (!CreateFullscreenPipeline(dev, window.surfaceFormat.format,
                                          descriptors.presentLayout, &fullscreen)) {
                break;
            }
            window.surfaceFormatChanged = false;   // cleared by whoever handled it
        }

        // Build what this frame draws.
        // --------------------------------------------------------------------
        //
        // Only the DrawItem array goes down to the recording layer: a matrix, an index
        // span, a pipeline, a texture. No transform and no geometry survive that far.
        //
        // What stays here is what changes per frame. aspect and proj do not.

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

        // Three ordering rules now, and the last two disagree.
        //   translucent last    correctness: depth write is off, so depth cannot order it
        //   group by pipeline   cost
        //   group by texture    cost
        //
        // This array groups by pipeline: 3 pipeline binds, 4 texture binds. Grouping by
        // texture reverses that. Choosing between them is where a sort key appears, and
        // with five objects there is nothing to measure.
        const glm::vec3 kZAxis{0.0f, 0.0f, 1.0f};
        const DrawItem items[] = {
            // green, spinning about z so its depth does not change
            {&pipeline, checker.set,
             {camera * glm::rotate(glm::mat4(1.0f), t, kZAxis), 1.0f}, kGreenIndices},

            // red, spinning the other way and slower
            {&pipeline, stripe.set,
             {camera * glm::rotate(glm::mat4(1.0f), -t * 0.5f, kZAxis), 1.0f}, kRedIndices},

            // quad, still
            {&pipeline, checker.set, {camera, 1.0f}, kQuadIndices},

            // green again as lines, over the same index span
            {&wireframe, checker.set,
             {camera * glm::scale(
                  glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, -0.6f, -0.5f)),
                  glm::vec3(0.5f)), 1.0f}, kGreenIndices},

            // translucent blue, and it has to be last
            {&translucent, stripe.set, {camera, 0.5f}, kBlueIndices},
        };

        // These break instead of continue. After the acquire, skipping the submit leaves
        // a signalled semaphore and a reset fence with nobody to wait on them.
        if (!RecordFrame(dev.table, frame.cmd, target, vertexBuffer, indexBuffer,
                         items, static_cast<uint32_t>(std::size(items)), fullscreen)) {
            break;
        }

        // Frame.h holds the reason submit and present are separate.
        if (!SubmitFrame(dev, frame, target.present->renderFinished)) {
            break;
        }
        if (!PresentFrame(dev, &window, *target.present)) {
            break;
        }

        frameIndex = (frameIndex + 1) % kFramesInFlight;
    }

    // Destructors run in reverse declaration order. This wait stays because
    // ~VulkanDevice waits only after every other destructor has already run, and the
    // swapchain and command pool may still be in use by the GPU.
    dev.table.vkDeviceWaitIdle(dev.handle);

    LOG("[vk] clean shutdown\n");
    return 0;
}
