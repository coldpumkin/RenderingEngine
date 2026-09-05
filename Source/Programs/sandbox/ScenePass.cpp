#include "ScenePass.h"

#include "Vulkan/Barrier.h"
#include "Vulkan/Mesh.h"

#include <cstring>    // memcpy
#include <iterator>   // std::size
#include <vector>     // one handle per material, counted at load time

#include <glm/matrix.hpp>   // inverse, transpose

SceneTargetDescs MakeSceneTargets(VkExtent2D extent, VkFormat colour,
                                  const TargetCapabilities& caps) noexcept {
    return SceneTargetDescs{
        // No SAMPLED: a sampler2D cannot read a multisample image.
        {extent, colour, caps.samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},

        // The only image that leaves this pass. SAMPLED because the post pass reads
        // it, TRANSFER_SRC because the capture does -- both bits are edges rather
        // than properties, and TRANSFER_SRC is always on because a flag set only in
        // capture builds would make the captured frame a different frame.
        {extent, colour, VK_SAMPLE_COUNT_1_BIT,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
             | VK_IMAGE_USAGE_SAMPLED_BIT
             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT},

        {extent, caps.depthFormat, caps.samples,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
    };
}

// One frame's three images, from the descs that say what they are.
bool CreateSceneTargets(const VulkanDevice& dev, const SceneTargetDescs& descs,
                        SceneTargets* out) noexcept {
    return CreateTexture(dev, descs.color, &out->color)
        && CreateTexture(dev, descs.resolve, &out->resolve)
        && CreateTexture(dev, descs.depth, &out->depth);
}

bool ResizeSceneTargets(const VulkanDevice& dev, const SceneTargetDescs& descs,
                        SceneTargets* out) noexcept {
    // Released before the new ones are asked for, and view-then-image inside each --
    // see ResetTexture. Assigning over them would destroy an image while a view made
    // from it is still alive.
    ResetTexture(&out->color);
    ResetTexture(&out->resolve);
    ResetTexture(&out->depth);

    if (!CreateSceneTargets(dev, descs, out)) {
        LOG("[vk] could not remake the scene targets at %ux%u\n",
            descs.color.extent.width, descs.color.extent.height);
        return false;
    }
    return true;
}

// Built without looking at the window, so this works while minimized - there may be
// no swapchain yet, and nothing here depends on one.
bool CreateScenePass(const Descriptors& descriptors,
                     const SceneTargets* const targets[kFramesInFlight],
                     const Mesh& mesh,
                     const Pipeline& pipeline, const Pipeline& wirePipeline,
                     const Texture* const shadowMaps[kFramesInFlight],
                     const FrameCamera* cameras, const FrameLight* lights,
                     const FrameShadow* shadows,
                     const Gui& gui, ScenePass* out) noexcept {
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

    out->pass.targets[0] = &targets[0]->color.desc;
    out->pass.targets[1] = &targets[0]->depth.desc;
    out->pass.uses[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    out->pass.uses[0].store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    out->pass.uses[0].clear.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};
    out->pass.uses[0].resolve = VK_RESOLVE_MODE_AVERAGE_BIT;
    // Clear 1.0 = farthest, paired with the pipeline's compareOp LESS.
    out->pass.uses[1].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    out->pass.uses[1].store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    out->pass.uses[1].clear.depthStencil.depth = 1.0f;
    out->wirePipeline = &wirePipeline;

    // Both variants have to answer to the same set layouts, or the sets filled below
    // fit one of them and not the other. Sharing a ShaderProgram is what guarantees
    // it, and comparing the two variants is the whole of what is left to ask: which
    // program either was built from is not something a caller can get wrong now.
    if (wirePipeline.program != pipeline.program) {
        LOG("[vk] the scene's two pipelines were built from different programs\n");
        return false;
    }


    // The bytes were written as one thing and are read as another unless these agree.
    // Nobody else looks: the pipeline checked its layout against the shader, the mesh
    // wrote its own, and the two only meet here.
    if (!SameVertexLayout(mesh.desc.vertexLayout, pipeline.vertexLayout)) {
        LOG("[vk] the mesh and this pass's pipeline disagree about the vertex layout\n");
        return false;
    }

    // Both variants, because both draw into these same images.
    // Every frame's targets against both pipelines, and so against each other.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        ScenePass::PerFrame& frame = out->frames[i];
        frame.targets = targets[i];

        // The multisample colour and the depth. The resolve is not here: it is not
        // an attachment, it is where EndRendering averages into, and no pipeline
        // bakes it.
        const AttachmentFormats formats = PassFormats(out->pass);
        if (!SameAttachmentFormats(formats, pipeline.formats)
            || !SameAttachmentFormats(formats, wirePipeline.formats)) {
            LOG("[vk] scene targets %u and a scene pipeline disagree about the formats\n",
                i);
            return false;
        }
    }

    // Drawn in one call, then handed out: vkAllocateDescriptorSets writes a flat
    // array and PerFrame is not one.
    VkDescriptorSet sets[kFramesInFlight]{};
    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], kFramesInFlight, sets)) {
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        ScenePass::PerFrame& frame = out->frames[i];
        frame.set = sets[i];

        // Four bindings, counted the same way: this frame's camera, its light, the
        // depth map the shadow pass drew for this same frame, and the panel's switches.
        // Frame for frame -- a set naming another slot's would read what the GPU is
        // still writing.
        //
        // Five, in the order the values are decided. 1 is what the light does to a
        // surface; 2 and 3 are the shadowing of it, and they are neighbours because
        // they are halves of one fact -- the matrix has to be the one that drew the
        // map beside it. That is not only a comment: binding 2 is the buffer the
        // shadow pass was handed, so the two cannot be different matrices unless
        // someone passes two different arrays.
        //
        // The last comes from a pass that draws after this one, which is the only edge
        // here that runs that direction. It is in this set for the same reason the
        // others are: one per frame in flight, and that is the whole rule for which set
        // a binding belongs in.
        const BindingValue values[] = {
            {nullptr, &cameras[i].buffer},
            {nullptr, &lights[i].buffer},
            {nullptr, &shadows[i].buffer},
            {&shadowMaps[i]->view},
            {nullptr, &GuiOptionsBuffer(gui, i)},
        };
        UpdateSet(descriptors, program.setLayouts[kFrameSet], frame.set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

bool CreateMaterials(const VulkanDevice& dev,
                     const Descriptors& descriptors, const DescriptorLayout& layout,
                     const MaterialDesc* sources, uint32_t count,
                     Material* out) noexcept {
    if (count == 0) { return true; }

    // One call, because the pool hands sets out in batches and a per-material call
    // would ask it 25 times for the same layout.
    std::vector<VkDescriptorSet> sets(count);
    if (!AllocateSets(descriptors, layout, count, sets.data())) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        out[i].set = sets[i];
        out[i].cullMode = sources[i].cullMode;   // copied, not bound: it is not a binding

        // 32 bytes, written once and never again -- but HOST_VISIBLE like the scene's
        // uniform rather than a staging copy, because a device-local upload for 32
        // bytes costs a command buffer and a queue wait each.
        if (!CreateBuffer(dev, sizeof(MaterialParams),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out[i].params)) {
            return false;
        }
        if (out[i].params.mapped == nullptr) {
            LOG("[vk] material uniform buffer is not mapped\n");
            return false;
        }
        std::memcpy(out[i].params.mapped, &sources[i].params, sizeof(MaterialParams));

        // Binding order, and the order is MaterialSet()'s -- the same declaration every
        // program that reads a material is checked against. The static_assert is what
        // keeps the two from drifting: a binding added there without a value here is a
        // set with a hole in it, which UpdateSet would fill from the wrong slot.
        const BindingValue values[] = {
            {&sources[i].baseColor->view},           // 0 baseColor
            {&sources[i].normal->view},              // 1 normalMap
            {nullptr, &out[i].params},               // 2 mtl
            {&sources[i].metallicRoughness->view},   // 3 metallicRoughnessMap
        };
        static_assert(std::size(values) == 4,
                      "one value per binding MaterialSet() declares");
        UpdateSet(descriptors, layout, out[i].set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

void RecordScenePass(const FrameSlot& slot, const ScenePass& scene,
                            const DrawList& draws, SceneRasterOptions raster,
                            DrawStats* stats) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const Mesh& mesh = *scene.mesh;

    // One choice for the whole pass, picked by what each variant was baked with rather
    // than by which field it sits in. A Pipeline records its polygonMode, so the choice
    // reads that instead of assuming wirePipeline is the LINE one -- and a third mode
    // would be a third variant here and no change to what the panel sends.
    const Pipeline& pipeline = raster.polygonMode == scene.wirePipeline->polygonMode
                                   ? *scene.wirePipeline : *scene.pipeline;

    // The layout comes from the pipeline that was just chosen, not from a program the
    // pass holds. What a draw receives -- which sets, which push range -- is the
    // pipeline's fact. A pass is a group of pipelines that agree about attachments,
    // which is all Vulkan asks of the group; that ours happen to share one program is
    // a property of these two variants and not of being in one pass.
    const VkPipelineLayout layout = pipeline.program->layout;

    // This slot's frame of the pass. The set that names these attachments is in the
    // same PerFrame, so the two cannot be picked apart by a wrong index.
    const ScenePass::PerFrame& frame = scene.frames[slot.index];
    const SceneTargets& targets = *frame.targets;
    const VkExtent2D extent = targets.color.desc.extent;   // render resolution, not window size

    // The resolve is not derived, and the reason is worth the four lines. It is a
    // second image the colour attachment names, written at EndRendering rather than by
    // any draw, and what makes UNDEFINED right for it is resolveMode covering the whole
    // render area -- not loadOp, which is the colour image's story. Deriving it from
    // the attachment would be getting the right answer from the wrong field.
    RecordLayoutTransition(vk, cmd, targets.resolve.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // The multisample colour, then the depth, in the order the desc declares them. The
    // resolve rides along beside the colour: vkCmdEndRendering does the averaging, so
    // there is no second pass and no vkCmdResolveImage. TOP_OF_PIPE because nothing
    // outside this command buffer holds any of these images.
    const Texture* const views[] = {&targets.color, &targets.depth};
    const Texture* const resolves[] = {&targets.resolve, nullptr};
    if (!BeginPass(vk, cmd, scene.pass, views, resolves,
                   VkRect2D{{0, 0}, extent}, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
        return;
    }

    // Up, because our world is y-up, and the pass is where that belongs: every draw in
    // here shares one viewport, and no pipeline had to be compiled knowing it.
    //
    // Five of these come from the panel. None of them is compiled in, so the whole
    // set costs one call per pass -- which is the difference between this and the
    // wireframe switch beside them, where polygonMode forced a second pipeline.
    //
    // cull is the starting value; the loop below changes it per draw unless the panel
    // overrode it.
    // The pipeline's own, with the panel's answers written over the four it owns. The
    // one pass of the four that overrides anything, and the only reason BindPipeline
    // has a second form.
    RasterState state = pipeline.raster;
    state.cull = raster.cull == kCullFromMaterial ? VK_CULL_MODE_NONE : raster.cull;
    state.depthTest = raster.depthTest ? VK_TRUE : VK_FALSE;
    state.depthWrite = raster.depthWrite ? VK_TRUE : VK_FALSE;
    state.depthCompare = raster.depthCompare;
    state.rasterizerDiscard = raster.rasterizerDiscard ? VK_TRUE : VK_FALSE;
    BindPipeline(vk, cmd, pipeline, VkRect2D{{0, 0}, extent}, state);

    // Once, above the loop: it is this frame's, and every draw in the pass reads it.
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                               kFrameSet, 1, &frame.set, 0, nullptr);

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
    // Two things change between draws and they do not change together: 89 of
    // Sponza's primitives are single sided and 14 are not, while the texture changes
    // far more often than that. Both are set only where two neighbours differ, which
    // is what a sort key would be sorting.
    //
    // Neither starting value is a real one: every cull mode is a legal state to begin
    // in, and every material index is a legal one to draw, so "not set yet" needs a
    // value outside both.
    //
    // Both read one index, so an item cannot ask for a cull mode its material does not
    // have. They still change at different rates: cull is a function of the material,
    // and a function is coarser than what it is a function of.
    uint32_t boundMaterial = kNoMaterial;
    VkCullModeFlags boundCull = UINT32_MAX;
    for (uint32_t i = 0; i < draws.itemCount; ++i) {
        const DrawItem& item = draws.items[i];

        // One comparison for two failures: kNoMaterial is above every valid index, so
        // an item nobody assigned a material and an index past the end are caught the
        // same way. The pointer this replaced could only report the first, and a stale
        // one not even that.
        if (item.material >= draws.materialCount) { continue; }
        const Material& material = draws.materials[item.material];

        // The material's, unless the panel overrode it. An override makes every draw
        // ask for the same value, so this fires once for the pass -- which is what the
        // panel's cull-change count shows.
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

        // viewProj is in the uniform this set already points at; only the item's own
        // values ride the command buffer.
        const PushConstants push{item.model,
                                 {item.normal[0], item.normal[1], item.normal[2]},
                                 item.alpha};
        vk.vkCmdPushConstants(cmd, layout,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(push), &push);

        // firstIndex is a position in the index buffer; vertexOffset is added to every
        // index it reads. Both come from the item because one buffer holds every
        // primitive's vertices and indices end to end.
        vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex,
                            item.vertexOffset, 0);
        if (stats != nullptr) { stats->draws += 1; }
    }

    vk.vkCmdEndRendering(cmd);
}
