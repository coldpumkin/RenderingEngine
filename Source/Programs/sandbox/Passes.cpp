#include "Passes.h"

#include "Vulkan/Barrier.h"
#include "Vulkan/Mesh.h"

#include <cstring>    // memcpy
#include <vector>     // one handle per material, counted at load time
#include <iterator>   // std::size

// Built without looking at the window, so this works while minimized - there may be
// no swapchain yet, and nothing here depends on one.
bool CreateScenePass(const VulkanDevice& dev, const Descriptors& descriptors,
                     AttachmentFormats formats, VkExtent2D extent,
                     const Mesh& mesh,
                     const Pipeline& pipeline, ScenePass* out) noexcept {
    out->mesh = &mesh;
    out->pipeline = &pipeline;

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        ScenePass::PerFrame& frame = out->frames[i];

        // Three descs, and every difference is written out rather than derived inside
        // CreateTexture: color is multisample and carries no SAMPLED (sampler2D cannot
        // read a multisample image), colorResolve is the 1-sample copy the post pass
        // reads, and depth never leaves the frame.
        if (!CreateTexture(dev, {extent, formats.color, formats.samples,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT}, &frame.color)) {
            return false;
        }
        if (!CreateTexture(dev, {extent, formats.color, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                     | VK_IMAGE_USAGE_SAMPLED_BIT},
                           &frame.colorResolve)) {
            return false;
        }
        if (!CreateTexture(dev, {extent, formats.depth, formats.samples,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
                           &frame.depth)) {
            return false;
        }

        // HOST_VISIBLE + MAPPED: one memcpy per frame, so there is no reason to go
        // through a staging buffer and a copy command.
        if (!CreateBuffer(dev, sizeof(SceneUniform),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &frame.uniform)) {
            return false;
        }
        if (frame.uniform.mapped == nullptr) {
            LOG("[vk] uniform buffer is not mapped\n");
            return false;
        }
    }

    // Drawn in one call, then handed out: vkAllocateDescriptorSets writes a flat
    // array and PerFrame is not one.
    VkDescriptorSet sets[kFramesInFlight]{};
    if (!AllocateSets(descriptors, pipeline.setLayouts[kFrameSet], kFramesInFlight, sets)) {
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        ScenePass::PerFrame& frame = out->frames[i];
        frame.set = sets[i];

        // One binding now. The texture that used to sit beside it is a material's,
        // and a material's set is counted by materials rather than by frames.
        const BindingValue values[] = {
            {VK_NULL_HANDLE, frame.uniform.handle, sizeof(SceneUniform)},  // 0: scene
        };
        UpdateSet(descriptors, pipeline.setLayouts[kFrameSet], frame.set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

bool CreateMaterials(const Descriptors& descriptors, const Pipeline& pipeline,
                     const Texture* textures, uint32_t count,
                     Material* out) noexcept {
    if (count == 0) { return true; }

    // One call, because the pool hands sets out in batches and a per-material call
    // would ask it 25 times for the same layout.
    std::vector<VkDescriptorSet> sets(count);
    if (!AllocateSets(descriptors, pipeline.setLayouts[kMaterialSet], count, sets.data())) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        out[i].set = sets[i];
        const BindingValue values[] = {{textures[i].image.view}};
        UpdateSet(descriptors, pipeline.setLayouts[kMaterialSet], out[i].set, values, 1);
    }
    return true;
}

bool CreatePostProcessPass(const Descriptors& descriptors, const ScenePass& source,
                           const Pipeline& pipeline, PostProcessPass* out) noexcept {
    out->source = &source;
    out->pipeline = &pipeline;

    if (!AllocateSets(descriptors, pipeline.setLayouts[kFrameSet], kFramesInFlight,
                      out->sets)) {
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        // The resolve, not color: a multisample image cannot be sampled.
        const BindingValue values[] = {{source.frames[i].colorResolve.image.view}};
        UpdateSet(descriptors, pipeline.setLayouts[kFrameSet], out->sets[i], values, 1);
    }
    return true;
}

// Scene pass
//
// Input:  the pass (attachments, mesh, texture, pipeline), the slot (cmd), and this
//         frame's draw list
// Effect: appends commands that draw into this slot's color / depth
//
// No swapchain, so this works without a window. No camera either: it went into the
// pass's uniform, which every draw here reads.
//
// One mesh for every item: the spans in items index into it. A second mesh means
// another BindVertexBuffers, which is why the bind sits above the loop and not in it.
static void RecordScenePass(const FrameSlot& slot, const ScenePass& scene,
                            const DrawItem* items, uint32_t itemCount) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const Mesh& mesh = *scene.mesh;
    const Pipeline& pipeline = *scene.pipeline;

    // This slot's frame of the pass. The set that names these attachments is in the
    // same PerFrame, so the two cannot be picked apart by a wrong index.
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

    // Not bound here any more -- each item names its own below. What is still the
    // pass's is the layout and the viewport sign, and every pipeline a draw can name
    // shares them.
    //
    // Once, above the loop: it is this frame's, and every draw in the pass reads it.
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
                               kFrameSet, 1, &targets.set, 0, nullptr);

    // binding 0 matches the pipeline's binding 0. offset changes once several meshes
    // share one buffer.
    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.handle, &offset);

    // Index buffers have no slot number: a command buffer holds exactly one.
    // Contract: this type must match the element type of indices.
    vk.vkCmdBindIndexBuffer(cmd, mesh.indices.handle, 0, mesh.desc.indexType);

    // Order is whatever the caller wrote into the array. This layer does not sort --
    // and now the order costs something: a material bind happens wherever two
    // neighbours differ, so the same items in another order bind more times.
    VkDescriptorSet boundMaterial = VK_NULL_HANDLE;
    const Pipeline* boundPipeline = nullptr;
    for (uint32_t i = 0; i < itemCount; ++i) {
        const DrawItem& item = items[i];

        // Two binds, two conditions. They are separate because they do not change
        // together: 89 of Sponza's primitives are opaque and 14 are masked, while its
        // textures change far more often than that.
        if (item.pipeline != boundPipeline) {
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 item.pipeline->handle);
            boundPipeline = item.pipeline;
        }

        if (item.material != boundMaterial) {
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       pipeline.layout, kMaterialSet, 1,
                                       &item.material, 0, nullptr);
            boundMaterial = item.material;
        }

        // viewProj is in the uniform this set already points at; only the item's own
        // values ride the command buffer.
        const PushConstants push{item.model, item.alpha, item.alphaCutoff};
        vk.vkCmdPushConstants(cmd, pipeline.layout,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(push), &push);

        // firstIndex is a position in the index buffer; vertexOffset is added to every
        // index it reads. Both come from the item because one buffer holds every
        // primitive's vertices and indices end to end.
        vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex,
                            item.vertexOffset, 0);
    }

    vk.vkCmdEndRendering(cmd);
}

// Post-process pass
//
// Input:  the pass (its source and pipeline), the slot (cmd, which frame), and the
//         texture to draw into
// Effect: appends commands that sample the scene pass's resolve into that texture
//
// A Texture, not the whole FrameTarget: nothing here reads the index or the semaphore,
// and those belong to getting the frame out, not to drawing it. Drawing somewhere else
// -- the next stage of an effect chain, a screenshot -- is then a different argument,
// not a different function.
static void RecordPostProcessPass(const FrameSlot& slot, const PostProcessPass& post,
                                  const Texture& target) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const Pipeline& pipeline = *post.pipeline;

    // What the scene pass left behind. The set bound below names this same image,
    // and both are picked by slot.index.
    const Texture& source = post.source->frames[slot.index].colorResolve;
    const Texture& dest = target;
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

    // oldLayout UNDEFINED for the same reason as the scene pass's colour: loadOp is
    // DONT_CARE below, so whatever the presentation engine left here is dead.
    //
    // srcStage must overlap SubmitFrame's wait stage, or this transition can run ahead
    // of the acquire.
    RecordLayoutTransition(vk, cmd, dest.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Window sized, unlike the scene pass. The sampler's LINEAR filter scales.
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

    // Opposite sign from the scene pass: this pipeline is built ViewportY::Down.
    const VkViewport viewport = MakeViewport(destExtent, pipeline.desc.viewportY);
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = destExtent;
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);

    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
                               0, 1, &post.sets[slot.index], 0, nullptr);

    // 3 vertices, no buffer. The shader builds them from gl_VertexIndex.
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

    // The one place this still assumes the target is a swapchain image. Drawing into
    // an intermediate texture would want SHADER_READ_ONLY here instead, and which one
    // it should be is the pass's output contract -- not written down anywhere yet.
    //
    // dstAccess is 0, unlike every other barrier here: present is not a queue
    // operation and reads nothing through the memory model, so there is no access to
    // make visible. The semaphore SubmitFrame signals is what present actually waits
    // on -- this barrier only has to leave the image in the right layout.
    RecordLayoutTransition(vk, cmd, dest.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

bool RecordFrame(const FrameSlot& slot, const ScenePass& scene,
                 const PostProcessPass& post, const Texture& target,
                 const DrawItem* items, uint32_t itemCount) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;

    // The value and its GPU copy meet here. Safe because BeginFrame waited on this
    // slot's fence, and this runs after it -- an acquired image is its precondition.
    const ScenePass::PerFrame& frame = scene.frames[slot.index];
    std::memcpy(frame.uniform.mapped, &frame.uniformValue, sizeof(frame.uniformValue));
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

    // The order is here, in these two lines, and nowhere else. post.source points at
    // scene, but that is a dependency -- it would not stop these from being swapped.
    RecordScenePass(slot, scene, items, itemCount);
    RecordPostProcessPass(slot, post, target);

    if (vk.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG("[vk] vkEndCommandBuffer failed\n");
        return false;
    }
    return true;
}
