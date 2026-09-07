#include "ShadowPass.h"

#include "Vulkan/Barrier.h"
#include "Vulkan/Mesh.h"

#include <iterator>   // std::size

TextureDesc MakeShadowTarget(const TargetCapabilities& caps) noexcept {
    // Square, and that is the technique's choice rather than a caller's.
    const VkExtent2D extent{kShadowResolution, kShadowResolution};

    // caps.samples goes unread on purpose -- see the header.
    TextureDesc desc{extent, caps.depthFormat, VK_SAMPLE_COUNT_1_BIT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT};

    // One layer per light rather than one image per light: they are the same size and
    // the same format, and a shader that shades light i wants to sample layer i without
    // the binding changing. An array is what "the same thing, N of them" is.
    desc.kind = TextureKind::Texture2DArray;
    desc.arrayLayers = kMaxLights;
    return desc;
}

bool CreateShadowPass(const Descriptors& descriptors,
                      const TextureDesc& mapDesc,
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

    // A pass draws into one layer, so what it declares is that layer -- a 2D map of the
    // array's size. The array is what the shading passes sample and is not what this
    // pass is about.
    out->layerDesc = SliceDesc(mapDesc);
    out->pass.attachments[0].resource = &out->layerDesc;

    // And what that layer is a layer of, which is what a later pass samples: nothing
    // reads one layer of a shadow array, it reads the array and picks.
    out->pass.attachments[0].whole = &mapDesc;
    out->pass.attachments[0].role = AttachmentRole::Depth;
    out->pass.attachments[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    out->pass.attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    out->pass.attachments[0].clear.depthStencil.depth = 1.0f;   // nothing seen is farther

    // Everything the declaration can be wrong about on its own, asked once here. What
    // needs a frame's images is asked every frame by BeginPass.
    if (!ValidatePassDesc(out->pass)) { return false; }

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

        if (!SameAttachmentFormats(PassFormats(out->pass), pipeline.formats)) {
            LOG("[vk] shadow map %u and its pipeline disagree about the formats\n", i);
            return false;
        }

        // One view per layer, kept because a frame draws through them every time. The
        // cube bakes make theirs and drop them, because those run once.
        for (uint32_t light = 0; light < kMaxLights; ++light) {
            ImageViewDesc viewDesc;
            viewDesc.type = VK_IMAGE_VIEW_TYPE_2D;
            viewDesc.baseLayer = light;
            viewDesc.layerCount = 1;
            if (!CreateImageView(*descriptors.dev, frame.depth->image.handle,
                                 mapDesc.format, mapDesc.samples, mapDesc.usage,
                                 viewDesc, &frame.layers[light])) {
                return false;
            }
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
                      const DrawList& draws, uint32_t casters) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const ShadowPass::PerFrame& frame = shadow.frames[slot.index];
    const VkExtent2D extent = frame.depth->desc.extent;
    const VkRect2D area{{0, 0}, extent};

    const Pipeline& pipeline = *shadow.pipeline;
    const VkPipelineLayout layout = pipeline.program->layout;
    const Mesh& mesh = *shadow.mesh;
    const VkDeviceSize offset = 0;

    // One map per casting light. The whole of what having several costs here is this
    // loop and the layer it draws into -- the pass, the pipeline and the draw list are
    // the same every time round, and only which matrices the vertex stage reads change.
    for (uint32_t light = 0; light < casters && light < kMaxLights; ++light) {
        const AttachmentView views[] = {
            {frame.depth->image.handle, &frame.layers[light], shadow.layerDesc}};
        if (!BeginPass(vk, cmd, shadow.pass, views, nullptr, area,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
            return;
        }

        BindPipeline(vk, cmd, pipeline, area);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   layout, kFrameSet, 1, &frame.set, 0, nullptr);
        vk.vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.handle, &offset);
        vk.vkCmdBindIndexBuffer(cmd, mesh.indices.handle, 0, mesh.desc.indexType);

        // Which of the maps this is, at offset 64 -- after the model matrix, because
        // that one changes per draw and this one per pass.
        const ShadowWhich which{static_cast<int32_t>(light)};
        vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                              sizeof(glm::mat4), sizeof(which), &which);

        for (uint32_t i = 0; i < draws.itemCount; ++i) {
            const DrawItem& item = draws.items[i];
            if (item.material >= draws.materialCount) { continue; }
            vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                                  0, sizeof(item.model), &item.model);
            vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex,
                                item.vertexOffset, 0);
        }
        vk.vkCmdEndRendering(cmd);

        // This layer, not the array. The ones no light drew were never moved out of
        // UNDEFINED, and a handover naming them would claim they are in a layout they
        // have never been in.
        RecordSampledHandover(vk, cmd, *frame.depth, AttachmentRole::Depth,
                              OneLayer(VK_IMAGE_ASPECT_DEPTH_BIT, light, 0));
    }

    // And the layers nothing drew, moved for a reason that is not about their contents.
    //
    // **A descriptor binds a view and the view is the whole array**, so every layer it
    // covers has to be in the layout the binding claims -- including the ones no light
    // used and no shader reads. UNDEFINED as the old layout is what says their contents
    // are worth nothing, which is true: nothing has ever been written there.
    const uint32_t drawn = casters < kMaxLights ? casters : kMaxLights;
    if (drawn < kMaxLights) {
        const VkImageSubresourceRange rest{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1,
                                           drawn, kMaxLights - drawn};
        RecordLayoutTransition(vk, cmd, frame.depth->image.handle, rest,
                               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}
