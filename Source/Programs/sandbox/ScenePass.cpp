#include "ScenePass.h"

#include "Vulkan/Barrier.h"
#include "Vulkan/Mesh.h"

#include <cstring>    // memcpy
#include <iterator>   // std::size
#include <vector>     // one handle per material, counted at load time

#include <glm/matrix.hpp>   // inverse, transpose

SceneTargetDescs MakeSceneTargets(VkExtent2D extent, VkFormat colour,
                                  const TargetCapabilities& caps) noexcept {
    SceneTargetDescs descs{};

    // No SAMPLED: a sampler2D cannot read a multisample image.
    descs.color = {extent, colour, caps.samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};

    // **From the colour, not written again beside it.** A resolve destination has to
    // match its source's format (VUID-VkRenderingAttachmentInfo-imageView-06865) and
    // cover the same area, and both were spelled out twice here -- the same shape as
    // the swapchain usage that a desc claimed and a swapchain granted separately.
    // What differs is what a resolve destination is for: one sample, and the two edges
    // out of this pass.
    //
    // SAMPLED because the post pass reads it, TRANSFER_SRC because the capture does.
    // TRANSFER_SRC is always on rather than only in capture builds, because a flag set
    // one way for a capture would make the captured frame a different frame.
    descs.resolve = descs.color;
    descs.resolve.samples = VK_SAMPLE_COUNT_1_BIT;
    descs.resolve.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                        | VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    descs.depth = {extent, caps.depthFormat, caps.samples,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    return descs;
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
                     const SceneTargetDescs& descs,
                     const SceneTargets* const targets[kFramesInFlight],
                     const Mesh& mesh,
                     const Pipeline& pipeline, const Pipeline& wirePipeline,
                     const FrameSetSources& frameSet, ScenePass* out) noexcept {
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

    out->pass.attachments[0].resource = &descs.color;
    out->pass.attachments[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    out->pass.attachments[0].store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    out->pass.attachments[0].clear.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    // Where the multisample colour is averaged into, said here rather than only handed
    // over at record time. It is the one image that leaves this pass -- both attachments
    // store DONT_CARE -- so it was the pass's only output with no declaration.
    out->pass.attachments[0].resolve = {&descs.resolve, VK_RESOLVE_MODE_AVERAGE_BIT};
    // Clear 1.0 = farthest, paired with the pipeline's compareOp LESS.
    out->pass.attachments[1].resource = &descs.depth;
    out->pass.attachments[1].role = AttachmentRole::Depth;
    out->pass.attachments[1].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    out->pass.attachments[1].store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    out->pass.attachments[1].clear.depthStencil.depth = 1.0f;

    // Everything the declaration can be wrong about on its own, asked once here. What
    // needs a frame's images is asked every frame by BeginPass.
    if (!ValidatePassDesc(out->pass)) { return false; }
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
    if (!SameVertexLayout(mesh.desc.vertexLayout, pipeline.vertexLayout)
        || !SameVertexLayout(mesh.desc.vertexLayout, wirePipeline.vertexLayout)) {
        LOG("[vk] the mesh and a scene pipeline disagree about the vertex layout\n");
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

    // The one image this pass reads. Nothing looked at it until 09-06: the argument
    // was named shadowMaps and that was the whole of what said it held shadow maps.
    // A colour image of the right shape would have gone in and drawn a wrong picture.
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
        if (!FillFrameSet(descriptors, program, frame.set, frameSet, i, &out->pass)) {
            return false;
        }
    }
    return true;
}


void RecordScenePass(const FrameSlot& slot, const ScenePass& scene,
                            const DrawList& draws, RasterOptions raster,
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

    // The multisample colour, then the depth, in the order the desc declares them. The
    // resolve rides along beside the colour: vkCmdEndRendering does the averaging, so
    // there is no second pass and no vkCmdResolveImage. TOP_OF_PIPE because nothing
    // outside this command buffer holds any of these images.
    const AttachmentView views[] = {TargetOf(targets.color), TargetOf(targets.depth)};
    const AttachmentView resolves[] = {TargetOf(targets.resolve), {}};
    if (!BeginPass(vk, cmd, scene.pass, views, resolves,
                   VkRect2D{{0, 0}, extent}, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
        return;
    }

    // Written whole rather than copied and overwritten, one line per owner. Every one
    // of these is dynamic state, so none of it was compiled in and the whole set costs
    // one call per pass -- which is the difference between these and the wireframe
    // switch beside them, where polygonMode forced a second pipeline.
    //
    // viewportY is the pipeline's: it follows the projection the world was built with,
    // and no checkbox reaches it. The five below are the panel's, and the panel is the
    // only place their defaults are written -- a second copy in the pipeline desc
    // would be a value nothing reads, and so a value nobody could catch being wrong.
    //
    // This is the one pass of the four that names any of them, and the only reason
    // BindPipeline has a second form.
    RasterState state{};
    state.viewportY = pipeline.raster.viewportY;

    // The starting value only; the loop below sets it per draw from each material,
    // unless the panel overrode it for every draw.
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
