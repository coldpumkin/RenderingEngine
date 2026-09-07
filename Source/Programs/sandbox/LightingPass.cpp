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
                        const GBufferTargetDescs& sourceDescs,
                        const GBufferTargets* const source[kFramesInFlight],
                        const TextureDesc& targetDesc,
                        const Texture* const target[kFramesInFlight],
                        const Pipeline& pipeline,
                        const FrameSetSources& frameSet,
                        LightingPass* out) noexcept {
    if (pipeline.program == nullptr) {
        LOG("[vk] a pass was given a pipeline that names no program\n");
        return false;
    }
    const ShaderProgram& program = *pipeline.program;

    out->pipeline = &pipeline;

    out->pass.attachments[0].resource = &targetDesc;
    // LOAD and not DONT_CARE: the sky pass fills this first and lighting.frag discards
    // where there is no geometry, so what was there has to survive.
    out->pass.attachments[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
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
    //
    // Each one is its resource and the images that stand for it per frame. DeclareRead
    // checks the kind the way this used to and keeps the identity, which is what the
    // frame graph is made of.
    PassInput albedo{&sourceDescs.albedo, {}};
    PassInput normal{&sourceDescs.normal, {}};
    PassInput material{&sourceDescs.material, {}};
    PassInput depth{&sourceDescs.depth, {}};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        albedo.frames[i] = &source[i]->albedo;
        normal.frames[i] = &source[i]->normal;
        material.frames[i] = &source[i]->material;
        depth.frames[i] = &source[i]->depth;
    }
    if (!DeclareRead(albedo, "g-buffer albedo", false, &out->pass)
            || !DeclareRead(normal, "g-buffer normal", false, &out->pass)
            || !DeclareRead(material, "g-buffer material", false, &out->pass)
            || !DeclareRead(depth, "g-buffer depth", true, &out->pass)) {
        return false;
    }

    // Every frame draws into an image of the same shape, which is what lets one
    // RenderPassDesc describe them all.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (target[i]->desc.format != targetDesc.format
                || target[i]->desc.samples != targetDesc.samples) {
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

        if (!FillFrameSet(descriptors, program, out->frameSets[i], frameSet, i,
                          &out->pass)) {
            return false;
        }

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
    // the resolve -- and it no longer states the writer's three values to do it. The
    // role it names is the one thing it knows and the one thing they follow from.
    const Texture* const colour[] = {&source.albedo, &source.normal, &source.material};
    for (uint32_t i = 0; i < std::size(colour); ++i) {
        RecordSampledHandover(vk, cmd, *colour[i], AttachmentRole::Color,
                              WholeImage(VK_IMAGE_ASPECT_COLOR_BIT));
    }
    RecordSampledHandover(vk, cmd, source.depth, AttachmentRole::Depth,
                          WholeImage(VK_IMAGE_ASPECT_DEPTH_BIT));

    // The shadow map needs none: the shadow pass published it at its own end, which is
    // the other of the two patterns and the one a writer can use when it knows every
    // reader wants the same thing.
    const AttachmentView views[] = {TargetOf(target)};
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
