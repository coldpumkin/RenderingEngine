#include "GeometryPass.h"

#include "Vulkan/Barrier.h"
#include "Vulkan/Mesh.h"

#include <iterator>   // std::size

GBufferTargetDescs MakeGBufferTargets(VkExtent2D extent, VkFormat albedo,
                                      const TargetCapabilities& caps) noexcept {
    // SAMPLED on all four, because all four are read by the pass after this one.
    // Every one of these bits is an edge in the frame rather than a property of the
    // image, which is the same thing the shadow map's SAMPLED says.
    constexpr VkImageUsageFlags kColour =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    return GBufferTargetDescs{
        // SRGB, matching the render chain's colour: what comes out of this is a
        // colour, and the hardware conversion is what makes the two paths' albedo the
        // same numbers.
        {extent, albedo, VK_SAMPLE_COUNT_1_BIT, kColour},

        // UNORM, not SRGB. A normal is a direction with a sign, folded into 0..1 by
        // the shader because the format cannot carry the sign -- putting it through
        // the SRGB curve as well would bend it, and nothing would say so. Eight bits
        // per axis is what makes this the one to widen if banding ever shows.
        {extent, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, kColour},

        // UNORM for the same reason: metallic and roughness are numbers, not colours.
        {extent, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, kColour},

        // The device's depth format, the same one the other passes use -- the lighting
        // pass reads this one and the scene pass never lets its own out.
        {extent, caps.depthFormat, VK_SAMPLE_COUNT_1_BIT,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
    };
}

bool CreateGBufferTargets(const VulkanDevice& dev, const GBufferTargetDescs& descs,
                          GBufferTargets* out) noexcept {
    return CreateTexture(dev, descs.albedo, &out->albedo)
        && CreateTexture(dev, descs.normal, &out->normal)
        && CreateTexture(dev, descs.material, &out->material)
        && CreateTexture(dev, descs.depth, &out->depth);
}

bool ResizeGBufferTargets(const VulkanDevice& dev, const GBufferTargetDescs& descs,
                          GBufferTargets* out) noexcept {
    ResetTexture(&out->albedo);
    ResetTexture(&out->normal);
    ResetTexture(&out->material);
    ResetTexture(&out->depth);

    if (!CreateGBufferTargets(dev, descs, out)) {
        LOG("[vk] could not remake the g-buffer at %ux%u\n",
            descs.albedo.extent.width, descs.albedo.extent.height);
        return false;
    }
    return true;
}

bool CreateGeometryPass(const Descriptors& descriptors,
                        const GBufferTargets* const targets[kFramesInFlight],
                        const Mesh& mesh,
                        const Pipeline& pipeline, const Pipeline& wirePipeline,
                        const FrameCamera* cameras,
                        const Gui& gui, GeometryPass* out) noexcept {
    if (pipeline.program == nullptr) {
        LOG("[vk] a pass was given a pipeline that names no program\n");
        return false;
    }
    const ShaderProgram& program = *pipeline.program;

    out->mesh = &mesh;
    out->pipeline = &pipeline;
    out->wirePipeline = &wirePipeline;

    // The order is geometry.frag's output order, and the depth last. uses[i] is what
    // happens to targets[i], so the two arrays are read together.
    out->pass.targets[0] = &targets[0]->albedo.desc;
    out->pass.targets[1] = &targets[0]->normal.desc;
    out->pass.targets[2] = &targets[0]->material.desc;
    out->pass.targets[3] = &targets[0]->depth.desc;

    for (uint32_t i = 0; i < 3; ++i) {
        out->pass.uses[i].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // STORE, where the scene pass is DONT_CARE. Its colour is resolved and thrown
        // away; these are the pass's product and the next pass reads them.
        out->pass.uses[i].store = VK_ATTACHMENT_STORE_OP_STORE;
        out->pass.uses[i].clear.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}};
    }

    // Clear 1.0 = farthest, paired with compareOp LESS. **STORE and not DONT_CARE**:
    // the lighting pass rebuilds a world position from this, so unlike the scene
    // pass's depth it does not end with the pass that wrote it.
    out->pass.uses[3].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    out->pass.uses[3].store = VK_ATTACHMENT_STORE_OP_STORE;
    out->pass.uses[3].clear.depthStencil.depth = 1.0f;

    if (wirePipeline.program != pipeline.program) {
        LOG("[vk] the geometry pass's two pipelines were built from different programs\n");
        return false;
    }

    if (!SameVertexLayout(mesh.desc.vertexLayout, pipeline.vertexLayout)
        || !SameVertexLayout(mesh.desc.vertexLayout, wirePipeline.vertexLayout)) {
        LOG("[vk] the mesh and a geometry pipeline disagree about the vertex layout\n");
        return false;
    }

    const AttachmentFormats formats = PassFormats(out->pass);
    if (!SameAttachmentFormats(formats, pipeline.formats)
        || !SameAttachmentFormats(formats, wirePipeline.formats)) {
        LOG("[vk] the g-buffer and a geometry pipeline disagree about the formats\n");
        return false;
    }

    VkDescriptorSet sets[kFramesInFlight]{};
    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], kFramesInFlight, sets)) {
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        GeometryPass::PerFrame& frame = out->frames[i];
        frame.targets = targets[i];
        frame.set = sets[i];

        // Five values for two bindings. This program declares set 0's binding 0 and
        // binding 4 and nothing between, so the layout has holes at 1..3 and UpdateSet
        // skips them -- but the array is still indexed by binding number, which is why
        // the three in the middle are here and empty. Writing only two would put the
        // panel's buffer in the light's slot.
        const BindingValue values[] = {
            {nullptr, &cameras[i].buffer},     // 0  camera, read by scene.vert
            {},                                // 1  light, not read here
            {},                                // 2  shadow matrix, not read here
            {},                                // 3  shadow map, not read here
            {nullptr, &GuiOptionsBuffer(gui, i)},   // 4  the panel
        };
        UpdateSet(descriptors, program.setLayouts[kFrameSet], frame.set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

void RecordGeometryPass(const FrameSlot& slot, const GeometryPass& geometry,
                        const DrawList& draws, RasterOptions raster,
                        DrawStats* stats) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const Mesh& mesh = *geometry.mesh;

    const Pipeline& pipeline = raster.polygonMode == geometry.wirePipeline->polygonMode
                                   ? *geometry.wirePipeline : *geometry.pipeline;
    const VkPipelineLayout layout = pipeline.program->layout;

    const GeometryPass::PerFrame& frame = geometry.frames[slot.index];
    const GBufferTargets& targets = *frame.targets;
    const VkExtent2D extent = targets.albedo.desc.extent;

    // No resolve to transition ahead of the pass, unlike the scene's: nothing here is
    // multisampled, so every image is an attachment BeginPass moves for itself.
    const Texture* const views[] = {&targets.albedo, &targets.normal,
                                    &targets.material, &targets.depth};
    if (!BeginPass(vk, cmd, geometry.pass, views, nullptr,
                   VkRect2D{{0, 0}, extent}, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
        return;
    }

    // The same six as the scene pass, from the same panel. viewportY is the
    // pipeline's, the five below are the panel's.
    RasterState state{};
    state.viewportY = pipeline.raster.viewportY;
    state.cull = raster.cull == kCullFromMaterial ? VK_CULL_MODE_NONE : raster.cull;
    state.depthTest = raster.depthTest ? VK_TRUE : VK_FALSE;
    state.depthWrite = raster.depthWrite ? VK_TRUE : VK_FALSE;
    state.depthCompare = raster.depthCompare;
    state.rasterizerDiscard = raster.rasterizerDiscard ? VK_TRUE : VK_FALSE;
    BindPipeline(vk, cmd, pipeline, VkRect2D{{0, 0}, extent}, state);

    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                               kFrameSet, 1, &frame.set, 0, nullptr);

    const VkDeviceSize offset = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.handle, &offset);
    vk.vkCmdBindIndexBuffer(cmd, mesh.indices.handle, 0, mesh.desc.indexType);

    // The same loop as the scene pass's, counted into the same DrawStats. That the two
    // numbers come out equal is the point: sorting buys what it buys whichever pass
    // walks the list, and a difference here would be a difference in this loop rather
    // than in deferred.
    uint32_t boundMaterial = kNoMaterial;
    VkCullModeFlags boundCull = UINT32_MAX;
    for (uint32_t i = 0; i < draws.itemCount; ++i) {
        const DrawItem& item = draws.items[i];
        if (item.material >= draws.materialCount) { continue; }
        const Material& material = draws.materials[item.material];

        const VkCullModeFlags wantCull =
            raster.cull == kCullFromMaterial ? material.cullMode : raster.cull;
        if (wantCull != boundCull) {
            vk.vkCmdSetCullMode(cmd, wantCull);
            boundCull = wantCull;
            if (stats != nullptr) { stats->cullChanges += 1; }
        }

        if (item.material != boundMaterial) {
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       layout, kMaterialSet, 1,
                                       &material.set, 0, nullptr);
            boundMaterial = item.material;
            if (stats != nullptr) { stats->materialBinds += 1; }
        }

        // The same block the scene pass pushes. geometry.frag reads only alpha, at the
        // offset the vertex stage's matrices leave it at -- a stage naming the part of
        // a block it reads is what the offsets are for.
        const PushConstants push{item.model,
                                 {item.normal[0], item.normal[1], item.normal[2]},
                                 item.alpha};
        vk.vkCmdPushConstants(cmd, layout,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(push), &push);

        vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex,
                            item.vertexOffset, 0);
        if (stats != nullptr) { stats->draws += 1; }
    }

    vk.vkCmdEndRendering(cmd);

    // The four images are left as attachments. Making them readable is the lighting
    // pass's first act, for the same reason the post pass transitions the resolve: a
    // barrier belongs where both sides of it are known, and the reader is the side
    // that knows what it is about to do.
}
