#include "ShadowPass.h"

#include "Vulkan/Barrier.h"
#include "Vulkan/Mesh.h"

#include <iterator>   // std::size

TextureDesc MakeShadowTarget(VkExtent2D extent, const TargetCapabilities& caps) noexcept {
    // caps.samples goes unread on purpose -- see the header.
    return TextureDesc{extent, caps.depthFormat, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT};
}

GraphicsPipelineDesc MakeShadowPipeline(const ShaderProgram& program,
                                        const VertexLayout& mesh,
                                        const TextureDesc& target) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    desc.vertexLayout = mesh;
    // One target, and its usage says it is the depth one. No colour follows, which is
    // what a program with no fragment stage means -- so blend[] stays empty and
    // polygonMode stays FILL.
    desc.targets[0] = &target;

    // ViewportY::Down settles the direction the map's v axis runs. scene.frag reads it
    // back as ndc * 0.5 + 0.5, which is this sign; the winding rides along and has no
    // effect on a pass that culls nothing.
    desc.raster.viewportY = ViewportY::Down;
    desc.raster.depthTest = VK_TRUE;
    desc.raster.depthWrite = VK_TRUE;
    return desc;
}

bool CreateShadowPass(const Descriptors& descriptors,
                      const Texture* const maps[kFramesInFlight],
                      const Mesh& mesh,
                      const Pipeline& pipeline, const FrameShadow* shadows,
                      ShadowPass* out) noexcept {
    // The program is the pipeline's, not a second argument beside it. A pipeline
    // records what it was built from, and taking both let a caller hand over a pair
    // that never met -- which is what the check below used to be for.
    if (pipeline.program == nullptr) {
        LOG("[vk] a pass was given a pipeline that names no program\n");
        return false;
    }
    const ShaderProgram& program = *pipeline.program;

    out->mesh = &mesh;
    out->pipeline = &pipeline;

    // The same comparison the scene pass makes, because both pipelines are built from
    // the same layout now. What differs between them is which locations their vertex
    // stages read, and that is the .spv's business rather than this one's.
    if (!SameVertexLayout(mesh.desc.vertexLayout, pipeline.vertexLayout)) {
        LOG("[vk] the mesh and the shadow pipeline disagree about the vertex layout\n");
        return false;
    }

    // Every map against the pipeline, which is also every map against every other.
    // They are made elsewhere and only meet the pipeline here; without this an image
    // could be made from a format the pipeline did not bake, and the first
    // vkCmdBeginRendering would say so at runtime instead of this saying so now.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        ShadowPass::PerFrame& frame = out->frames[i];
        frame.depth = maps[i];

        const TextureDesc* const drawnInto[] = {&maps[i]->desc};
        if (!SameAttachmentFormats(AttachmentFormatsOf(drawnInto, 1),
                                   pipeline.formats)) {
            LOG("[vk] shadow map %u and its pipeline disagree about the formats\n", i);
            return false;
        }
    }

    VkDescriptorSet sets[kFramesInFlight]{};
    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], kFramesInFlight, sets)) {
        return false;
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        ShadowPass::PerFrame& frame = out->frames[i];
        frame.set = sets[i];
        // The matrix, handed in. One field, and this stage reads all of it -- the
        // direction and colour that used to sit behind it are the scene pass's alone
        // and live in their own buffer now.
        const BindingValue values[] = {
            {nullptr, &shadows[i].buffer},
        };
        UpdateSet(descriptors, program.setLayouts[kFrameSet], frame.set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

// One mesh for every item: the spans in items index into it. A second mesh means
// another BindVertexBuffers, which is why the bind sits above the loop and not in it.
void RecordShadowPass(const FrameSlot& slot, const ShadowPass& shadow,
                      const DrawList& draws) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const ShadowPass::PerFrame& frame = shadow.frames[slot.index];
    const VkExtent2D extent = frame.depth->desc.extent;

    // The one image this pass draws into, and everything that happens to it.
    //
    // loadOp CLEAR: the last frame's map is spent, and the image was left
    // SHADER_READ_ONLY by the frame before -- discarding that is what CLEAR means here.
    // storeOp STORE, unlike the scene pass's depth: this one is the product.
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = frame.depth->view.handle;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = 1.0f;   // nothing seen yet is farthest

    // Read out of the attachment above rather than written again: CLEAR is what makes
    // the old contents dead, and DEPTH_ATTACHMENT_OPTIMAL is what says which stage
    // writes it. TOP_OF_PIPE because nothing outside this command buffer holds the map.
    RecordAttachmentTransition(vk, cmd, frame.depth->image.handle,
                               VK_IMAGE_ASPECT_DEPTH_BIT, depth,
                               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);

    // colorAttachmentCount 0 and no pColorAttachments. The pipeline was compiled the
    // same way, from a fragment stage that declares no outputs.
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;

    vk.vkCmdBeginRendering(cmd, &rendering);

    // The layout comes from the pipeline that is about to be bound, not from a program
    // the pass holds. What a draw receives -- which sets, which push range -- is the
    // pipeline's fact; a pass is a group of pipelines that agree about attachments,
    // and Vulkan asks for nothing more than that of the group.
    const Pipeline& pipeline = *shadow.pipeline;
    const VkPipelineLayout layout = pipeline.program->layout;

    // Its own raster state comes with it. What that state is and why is at
    // MakeShadowPipeline, where the rest of what this pipeline is made of already is.
    BindPipeline(vk, cmd, pipeline, VkRect2D{{0, 0}, extent});
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               layout, kFrameSet, 1, &frame.set,
                               0, nullptr);

    const Mesh& mesh = *shadow.mesh;
    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.handle, &offset);
    vk.vkCmdBindIndexBuffer(cmd, mesh.indices.handle, 0, mesh.desc.indexType);

    for (uint32_t i = 0; i < draws.itemCount; ++i) {
        const DrawItem& item = draws.items[i];
        if (item.material >= draws.materialCount) { continue; }

        // The model matrix alone. shadow.vert declares the front of the same block the
        // scene shaders declare all of, so the offset is shared and the size is not.
        vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                              0, sizeof(item.model), &item.model);
        vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex,
                            item.vertexOffset, 0);
    }

    vk.vkCmdEndRendering(cmd);

    // Handed over here rather than at the top of the scene pass. The pass that wrote
    // an image is what knows when it stopped writing, and this keeps the scene pass
    // from having to name a pass it only reads through a descriptor.
    //
    // The three values passed are this pass's own -- where it stopped writing, and the
    // layout it wrote in. Nothing about the reader is named here.
    RecordSampledTransition(vk, cmd, frame.depth->image.handle, VK_IMAGE_ASPECT_DEPTH_BIT,
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
}
