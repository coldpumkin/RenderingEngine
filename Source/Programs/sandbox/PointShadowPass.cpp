#include "PointShadowPass.h"

#include "Config.h"
#include "Vulkan/Barrier.h"
#include "Vulkan/Mesh.h"

#include <iterator>   // std::size

TextureDesc MakePointShadowTarget() noexcept {
    TextureDesc desc{};
    desc.extent = VkExtent2D{kPointShadowResolution, kPointShadowResolution};

    // Half a float per texel. What is written is already divided by the light's range,
    // so the values are 0..1 and the precision left over is more than a shadow test can
    // use.
    desc.format = VK_FORMAT_R16_SFLOAT;
    desc.samples = VK_SAMPLE_COUNT_1_BIT;

    // Drawn into a face at a time, then read as a whole cube array.
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    desc.kind = TextureKind::CubeArray;
    desc.arrayLayers = kMaxLights;   // cubes, not layers
    return desc;
}

TextureDesc MakePointShadowDepth(const TargetCapabilities& caps) noexcept {
    TextureDesc desc{};
    desc.extent = VkExtent2D{kPointShadowResolution, kPointShadowResolution};
    desc.format = caps.depthFormat;

    // One sample. Averaging depths across an edge produces a value no surface was at,
    // and every comparison against it is wrong -- the same reason the 2D map refuses the
    // device's sample count.
    desc.samples = VK_SAMPLE_COUNT_1_BIT;

    // Attachment only. Nothing samples it: it exists so the nearest surface wins.
    desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    desc.kind = TextureKind::Texture2DArray;
    desc.arrayLayers = kMaxLights * 6;
    return desc;
}

bool CreatePointShadowPass(const Descriptors& descriptors,
                           const TextureDesc& cubeDesc,
                           const Texture* const cubes[kFramesInFlight],
                           const TextureDesc& depthDesc,
                           const Texture* const depths[kFramesInFlight],
                           const Mesh& mesh,
                           const Pipeline& pipeline,
                           const FramePointShadow* shadows,
                           PointShadowPass* out) noexcept {
    if (pipeline.program == nullptr) {
        LOG("[vk] a pass was given a pipeline that names no program\n");
        return false;
    }
    const ShaderProgram& program = *pipeline.program;

    out->mesh = &mesh;
    out->pipeline = &pipeline;

    // What one face is: a 2D target of the cube's size. The array is what the shading
    // passes sample and is not what this pass is about.
    out->faceDesc = SliceDesc(cubeDesc);
    out->depthFaceDesc = SliceDesc(depthDesc);

    out->pass.attachments[0].resource = &out->faceDesc;
    // The caller's desc, not a copy of it: what makes this an edge is that the pass
    // that samples the cube points at the same TextureDesc.
    out->pass.attachments[0].whole = &cubeDesc;
    out->pass.attachments[0].role = AttachmentRole::Color;
    out->pass.attachments[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    out->pass.attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;

    // 1.0 is the far end of the range, which is what "nothing between here and the
    // light" comes to once the distance is divided by it.
    out->pass.attachments[0].clear.color.float32[0] = 1.0f;

    out->pass.attachments[1].resource = &out->depthFaceDesc;
    out->pass.attachments[1].role = AttachmentRole::Depth;
    out->pass.attachments[1].load = VK_ATTACHMENT_LOAD_OP_CLEAR;

    // DONT_CARE: the depth decides which surface wins inside this face and nothing
    // outside the pass ever looks at it.
    out->pass.attachments[1].store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    out->pass.attachments[1].clear.depthStencil.depth = 1.0f;

    if (!ValidatePassDesc(out->pass)) { return false; }

    if (!SameVertexLayout(mesh.desc.vertexLayout, pipeline.vertexLayout)) {
        LOG("[vk] the mesh and the point shadow pipeline disagree about the layout\n");
        return false;
    }
    if (!SameAttachmentFormats(PassFormats(out->pass), pipeline.formats)) {
        LOG("[vk] the point shadow cube and its pipeline disagree about the formats\n");
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        PointShadowPass::PerFrame& frame = out->frames[i];
        frame.cube = cubes[i];
        frame.depth = depths[i];

        // One view per face, and the depth layer that goes with it. Six per light, in
        // the order the cube is addressed: light L owns 6L .. 6L+5.
        for (uint32_t face = 0; face < kMaxLights * 6; ++face) {
            ImageViewDesc viewDesc;
            viewDesc.type = VK_IMAGE_VIEW_TYPE_2D;
            viewDesc.baseLayer = face;
            viewDesc.layerCount = 1;
            if (!CreateImageView(*descriptors.dev, frame.cube->image.handle,
                                 cubeDesc.format, cubeDesc.samples, cubeDesc.usage,
                                 viewDesc, &frame.faces[face])) {
                return false;
            }
            if (!CreateImageView(*descriptors.dev, frame.depth->image.handle,
                                 depthDesc.format, depthDesc.samples, depthDesc.usage,
                                 viewDesc, &frame.depthFaces[face])) {
                return false;
            }
        }
    }

    VkDescriptorSet sets[kFramesInFlight]{};
    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], kFramesInFlight,
                      sets)) {
        return false;
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        PointShadowPass::PerFrame& frame = out->frames[i];
        frame.set = sets[i];

        // The matrices and where each light is, in one buffer that both stages read.
        const BindingValue values[] = {
            {nullptr, &shadows[i].buffer},
        };
        UpdateSet(descriptors, program.setLayouts[kFrameSet], frame.set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

void RecordPointShadowPass(const FrameSlot& slot, const PointShadowPass& shadow,
                           const DrawList& draws,
                           const uint32_t* lights, uint32_t count) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const PointShadowPass::PerFrame& frame = shadow.frames[slot.index];
    const VkRect2D area{{0, 0}, frame.cube->desc.extent};

    const Pipeline& pipeline = *shadow.pipeline;
    const VkPipelineLayout layout = pipeline.program->layout;
    const Mesh& mesh = *shadow.mesh;
    const VkDeviceSize offset = 0;

    bool drawn[kMaxLights]{};

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t light = lights[i];
        if (light >= kMaxLights) { continue; }
        drawn[light] = true;

        for (uint32_t face = 0; face < 6; ++face) {
            const uint32_t slice = light * 6 + face;
            const AttachmentView views[] = {
                {frame.cube->image.handle, &frame.faces[slice], shadow.faceDesc},
                {frame.depth->image.handle, &frame.depthFaces[slice],
                 shadow.depthFaceDesc},
            };
            if (!BeginPass(vk, cmd, shadow.pass, views, nullptr, area,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
                return;
            }

            BindPipeline(vk, cmd, pipeline, area);
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       layout, kFrameSet, 1, &frame.set, 0, nullptr);
            vk.vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.handle, &offset);
            vk.vkCmdBindIndexBuffer(cmd, mesh.indices.handle, 0, mesh.desc.indexType);

            // Which light and which of its faces, at offset 64 -- after the model
            // matrix, because that one changes per draw and these per pass. Both stages
            // read it: the vertex stage picks the matrix, the fragment stage the light's
            // position.
            const PointShadowWhich which{static_cast<int32_t>(light),
                                         static_cast<int32_t>(face)};
            vk.vkCmdPushConstants(cmd, layout,
                                  VK_SHADER_STAGE_VERTEX_BIT
                                      | VK_SHADER_STAGE_FRAGMENT_BIT,
                                  sizeof(glm::mat4), sizeof(which), &which);

            for (uint32_t item = 0; item < draws.itemCount; ++item) {
                const DrawItem& draw = draws.items[item];
                if (draw.material >= draws.materialCount) { continue; }
                // Both stages, even though only the vertex one reads the model matrix.
                // The two declare overlapping parts of one block, so the layout holds a
                // single range covering both, and a push naming fewer stages than the
                // range does is a validation error rather than a narrower write.
                vk.vkCmdPushConstants(cmd, layout,
                                      VK_SHADER_STAGE_VERTEX_BIT
                                          | VK_SHADER_STAGE_FRAGMENT_BIT,
                                      0, sizeof(draw.model), &draw.model);
                vk.vkCmdDrawIndexed(cmd, draw.range.count, 1, draw.range.firstIndex,
                                    draw.vertexOffset, 0);
            }
            vk.vkCmdEndRendering(cmd);
        }

        // This light's six faces, not the array. The slots no light drew were never
        // moved out of UNDEFINED, and a handover naming them would claim they are in a
        // layout they have never been in.
        RecordSampledHandover(vk, cmd, *frame.cube, AttachmentRole::Color,
                              VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                                      light * 6, 6});
    }

    // And the slots nothing drew. A descriptor binds a view and the view is the whole
    // array, so every layer it covers has to be in the layout the binding claims --
    // including the ones no light used. UNDEFINED as the old layout is what says their
    // contents are worth nothing, which is true.
    for (uint32_t light = 0; light < kMaxLights; ++light) {
        if (drawn[light]) { continue; }
        const VkImageSubresourceRange rest{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                           light * 6, 6};
        RecordLayoutTransition(vk, cmd, frame.cube->image.handle, rest,
                               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}
