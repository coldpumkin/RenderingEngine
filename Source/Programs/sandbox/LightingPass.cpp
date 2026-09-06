#include "LightingPass.h"

#include "Vulkan/Barrier.h"

#include <iterator>   // std::size

// The four views a g-buffer set names, in lighting.frag's binding order.
static void FillGBufferSet(const Descriptors& descriptors, const DescriptorLayout& layout,
                           VkDescriptorSet set, const GBufferTargets& targets) noexcept {
    const BindingValue values[] = {
        {&targets.albedo.view},     // 0 gAlbedo
        {&targets.normal.view},     // 1 gNormal
        {&targets.material.view},   // 2 gMaterial
        {&targets.depth.view},      // 3 gDepth
    };
    static_assert(std::size(values) == 4,
                  "one value per binding lighting.frag declares in set 1");
    UpdateSet(descriptors, layout, set, values, static_cast<uint32_t>(std::size(values)));
}

void RefreshLightingPass(const Descriptors& descriptors, LightingPass* lighting) noexcept {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        FillGBufferSet(descriptors,
                       lighting->pipeline->program->setLayouts[kMaterialSet],
                       lighting->gbufferSets[i], *lighting->source[i]);
    }
}

bool CreateLightingPass(const Descriptors& descriptors,
                        const GBufferTargets* const source[kFramesInFlight],
                        const Texture* const target[kFramesInFlight],
                        const Pipeline& pipeline,
                        const Texture* const shadowMaps[kFramesInFlight],
                        const FrameCamera* cameras, const FrameLight* lights,
                        const FrameShadow* shadows,
                        const FrameViewOptions* views, LightingPass* out) noexcept {
    if (pipeline.program == nullptr) {
        LOG("[vk] a pass was given a pipeline that names no program\n");
        return false;
    }
    const ShaderProgram& program = *pipeline.program;

    out->pipeline = &pipeline;

    out->pass.attachments[0].resource = &target[0]->desc;
    out->pass.attachments[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    out->pass.attachments[0].store = VK_ATTACHMENT_STORE_OP_STORE;

    // Everything the declaration can be wrong about on its own, asked once here. What
    // needs a frame's images is asked every frame by BeginPass.
    if (!ValidatePassDesc(out->pass)) { return false; }

    if (!SameAttachmentFormats(PassFormats(out->pass), pipeline.formats)) {
        LOG("[vk] the lighting pass's target and its pipeline disagree about the formats\n");
        return false;
    }

    // The four it reads, in lighting.frag's binding order, and the last is a depth --
    // which is the whole shape of this pass: three colours describing a surface and a
    // depth the position is rebuilt from. Reading that one as a colour would be legal
    // Vulkan and a wrong picture, so the kind is stated here rather than assumed.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CheckSampledInput(source[i]->albedo.desc, "g-buffer albedo",
                               VK_IMAGE_ASPECT_COLOR_BIT)
                || !CheckSampledInput(source[i]->normal.desc, "g-buffer normal",
                                      VK_IMAGE_ASPECT_COLOR_BIT)
                || !CheckSampledInput(source[i]->material.desc, "g-buffer material",
                                      VK_IMAGE_ASPECT_COLOR_BIT)
                || !CheckSampledInput(source[i]->depth.desc, "g-buffer depth",
                                      VK_IMAGE_ASPECT_DEPTH_BIT)) {
            return false;
        }
        // Every frame draws into an image of the same shape, which is what lets one
        // RenderPassDesc describe them all -- targets[0] above is frame 0's desc.
        if (target[i]->desc.format != target[0]->desc.format
                || target[i]->desc.samples != target[0]->desc.samples) {
            LOG("[vk] the lighting pass's targets are not all the same kind\n");
            return false;
        }
    }

    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], kFramesInFlight,
                      out->frameSets)
            || !AllocateSets(descriptors, program.setLayouts[kMaterialSet],
                             kFramesInFlight, out->gbufferSets)) {
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        out->source[i] = source[i];
        out->target[i] = target[i];

        // Set 0, the same five the scene pass fills in the same order. That they are
        // the same five is not a coincidence to be preserved by hand -- lighting.frag
        // declares them so that the forward and the deferred lighting read one frame.
        const BindingValue frame[] = {
            {nullptr, &cameras[i].buffer},
            {nullptr, &lights[i].buffer},
            {nullptr, &shadows[i].buffer},
            {&shadowMaps[i]->view},
            {nullptr, &views[i].buffer},
        };
        UpdateSet(descriptors, program.setLayouts[kFrameSet], out->frameSets[i],
                  frame, static_cast<uint32_t>(std::size(frame)));

        // Set 1, written in the same step as the pointer above it so the two cannot
        // come to disagree about which g-buffer frame i reads.
        FillGBufferSet(descriptors, program.setLayouts[kMaterialSet],
                       out->gbufferSets[i], *source[i]);
    }
    return true;
}

void RecordLightingPass(const FrameSlot& slot, const LightingPass& lighting) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const Pipeline& pipeline = *lighting.pipeline;
    const VkPipelineLayout layout = pipeline.program->layout;

    const GBufferTargets& source = *lighting.source[slot.index];
    const Texture& target = *lighting.target[slot.index];
    const VkExtent2D extent = target.desc.extent;

    // The four images this pass reads, moved from what the geometry pass left them as
    // to what a sampler needs. The reader issues them, the way the post pass does for
    // the resolve: the three values passed are the writer's and had to be told.
    //
    // The three colours share a stage and the depth does not -- a depth attachment is
    // written by the late fragment tests, not by colour output -- which is the whole
    // reason this is four calls and not a loop over four images.
    const Texture* const colour[] = {&source.albedo, &source.normal, &source.material};
    for (uint32_t i = 0; i < std::size(colour); ++i) {
        RecordSampledTransition(vk, cmd, colour[i]->image.handle,
                                VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    RecordSampledTransition(vk, cmd, source.depth.image.handle, VK_IMAGE_ASPECT_DEPTH_BIT,
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    // The shadow map needs none: the shadow pass published it at its own end, which is
    // the other of the two patterns and the one a writer can use when it knows every
    // reader wants the same thing.
    const Texture* const views[] = {&target};
    if (!BeginPass(vk, cmd, lighting.pass, views, nullptr,
                   VkRect2D{{0, 0}, extent}, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
        return;
    }

    // No override. Every raster switch the panel has is about drawing surfaces, and
    // this draw is one triangle with the depth test off -- so the pipeline's own state
    // is the answer, which is what the one-argument form means.
    BindPipeline(vk, cmd, pipeline, VkRect2D{{0, 0}, extent});

    // Both sets, in one call: they are contiguous from set 0 and the array is in set
    // order, so firstSet is 0 and the count is what says which two.
    const VkDescriptorSet sets[] = {lighting.frameSets[slot.index],
                                    lighting.gbufferSets[slot.index]};
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                               kFrameSet, static_cast<uint32_t>(std::size(sets)),
                               sets, 0, nullptr);

    // 3 vertices, no buffer. The shader builds them from gl_VertexIndex.
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

    // Left COLOR_ATTACHMENT_OPTIMAL, which is what the scene pass leaves its resolve
    // as. The post pass transitions it either way and cannot tell which ran.
}
