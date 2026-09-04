#include "Passes.h"

#include "Gui.h"
#include "Vulkan/Barrier.h"
#include "Vulkan/Mesh.h"

#include <cstring>    // memcpy
#include <vector>     // one handle per material, counted at load time
#include <iterator>   // std::size

#include <glm/matrix.hpp>   // inverse, transpose

// HOST_VISIBLE + MAPPED, like every uniform here: one memcpy a frame, so a staging
// buffer and a copy command would buy nothing.
bool CreateFrameLights(const VulkanDevice& dev, FrameLight* out) noexcept {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateBuffer(dev, sizeof(LightUniform),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &out[i].buffer)) {
            return false;
        }
        if (out[i].buffer.mapped == nullptr) {
            LOG("[vk] light uniform buffer is not mapped\n");
            return false;
        }
    }
    return true;
}

bool CreateShadowPass(const VulkanDevice& dev, const Descriptors& descriptors,
                      VkExtent2D extent,
                      const Mesh& mesh, const ShaderProgram& program,
                      const Pipeline& pipeline, const FrameLight* lights,
                      ShadowPass* out) noexcept {
    out->mesh = &mesh;
    out->program = &program;
    out->pipeline = &pipeline;

    // The one place these come from. A pipeline bakes them in, so asking it is asking
    // the thing the images have to match.
    const AttachmentFormats& formats = pipeline.desc.formats;

    // The same comparison the scene pass makes, because both pipelines are built from
    // the same layout now. What differs between them is which locations their vertex
    // stages read, and that is the .spv's business rather than this one's.
    if (!SameVertexLayout(mesh.desc.vertexLayout, pipeline.desc.vertexLayout)) {
        LOG("[vk] the mesh and the shadow pipeline disagree about the vertex layout\n");
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        ShadowPass::PerFrame& frame = out->frames[i];

        // Both usages, which is what makes this image the seam between two passes.
        // One sample: averaging depths across an edge produces a value no surface was
        // ever at, and every fragment comparing against it is wrong.
        if (!CreateTexture(dev, {extent, formats.depth, formats.samples,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                     | VK_IMAGE_USAGE_SAMPLED_BIT},
                           &frame.depth)) {
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
        // The light, handed in. shadow.vert declares only its first field, so the
        // range covers more than this stage reads -- which is what a descriptor over a
        // shared buffer looks like and not a mistake.
        const BindingValue values[] = {
            {VK_NULL_HANDLE, lights[i].buffer.handle, sizeof(LightUniform)},
        };
        UpdateSet(descriptors, program.setLayouts[kFrameSet], frame.set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

// Built without looking at the window, so this works while minimized - there may be
// no swapchain yet, and nothing here depends on one.
bool CreateScenePass(const VulkanDevice& dev, const Descriptors& descriptors,
                     VkExtent2D extent,
                     const Mesh& mesh, const ShaderProgram& program,
                     const Pipeline& pipeline, const Pipeline& wirePipeline,
                     const Texture* const shadowMaps[kFramesInFlight],
                     const FrameLight* lights,
                     const Gui& gui, ScenePass* out) noexcept {
    out->mesh = &mesh;
    out->program = &program;
    out->pipeline = &pipeline;
    out->wirePipeline = &wirePipeline;

    // Both variants have to answer to the same set layouts, or the sets filled below
    // fit one of them and not the other. Sharing a ShaderProgram is what guarantees
    // it, and this is the line that says so out loud.
    if (pipeline.program != &program || wirePipeline.program != &program) {
        LOG("[vk] a scene pipeline was built from a different program\n");
        return false;
    }

    // Read off the pipeline, like the shadow pass above. The images below exist
    // because these three values say so: a colour format, a sample count above one,
    // and a depth format.
    const AttachmentFormats& formats = pipeline.desc.formats;

    // The bytes were written as one thing and are read as another unless these agree.
    // Nobody else looks: the pipeline checked its layout against the shader, the mesh
    // wrote its own, and the two only meet here.
    if (!SameVertexLayout(mesh.desc.vertexLayout, pipeline.desc.vertexLayout)) {
        LOG("[vk] the mesh and this pass's pipeline disagree about the vertex layout\n");
        return false;
    }

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
        // TRANSFER_SRC is for reading it back: this is the one image in the frame that
        // is both what the scene produced and 1-sample, so it is the only one a
        // capture can copy. Always on rather than behind a switch -- a flag that is
        // only set in capture builds makes the captured frame a different frame.
        if (!CreateTexture(dev, {extent, formats.color, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                     | VK_IMAGE_USAGE_SAMPLED_BIT
                                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT},
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
        //
        // The camera only. The light is FrameLight's -- the second pass that wanted it
        // is why it is not here.
        if (!CreateBuffer(dev, sizeof(CameraUniform),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                              | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                          &frame.cameraUniform)) {
            return false;
        }
        if (frame.cameraUniform.mapped == nullptr) {
            LOG("[vk] the camera uniform buffer is not mapped\n");
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
        // 1 and 2 are neighbours because they are halves of one fact: the matrix has to
        // be the one that drew the map beside it. That is no longer only a comment --
        // binding 1 is the buffer the shadow pass was handed, so the two cannot be
        // different matrices unless someone passes two different arrays.
        //
        // The last comes from a pass that draws after this one, which is the only edge
        // here that runs that direction. It is in this set for the same reason the
        // others are: one per frame in flight, and that is the whole rule for which set
        // a binding belongs in.
        const BindingValue values[] = {
            {VK_NULL_HANDLE, frame.cameraUniform.handle, sizeof(CameraUniform)},
            {VK_NULL_HANDLE, lights[i].buffer.handle, sizeof(LightUniform)},
            {shadowMaps[i]->view.handle, VK_NULL_HANDLE, 0},
            {VK_NULL_HANDLE, GuiOptionsBuffer(gui, i), kGuiOptionsSize},
        };
        UpdateSet(descriptors, program.setLayouts[kFrameSet], frame.set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

void SetDrawModel(DrawItem* item, const glm::mat4& model) noexcept {
    item->model = model;

    // The inverse-transpose of the upper 3x3. For a rotation it is the same matrix,
    // and for a uniform scale it differs only in length -- which the fragment stage
    // normalizes away. It earns its place the moment a scale is not uniform, and
    // nothing here would have said so.
    const glm::mat3 normal = glm::transpose(glm::inverse(glm::mat3(model)));
    item->normal[0] = glm::vec4{normal[0], 0.0f};
    item->normal[1] = glm::vec4{normal[1], 0.0f};
    item->normal[2] = glm::vec4{normal[2], 0.0f};
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

        // Order is binding order, which the shader declares and reflection reports.
        const BindingValue values[] = {
            {sources[i].baseColor->view.handle},                              // 0
            {sources[i].normal->view.handle},                                 // 1
            {VK_NULL_HANDLE, out[i].params.handle, sizeof(MaterialParams)},   // 2
            {sources[i].metallicRoughness->view.handle},                      // 3
        };
        UpdateSet(descriptors, layout, out[i].set,
                  values, static_cast<uint32_t>(std::size(values)));
    }
    return true;
}

bool CreatePostProcessPass(const Descriptors& descriptors,
                           const Texture* const source[kFramesInFlight],
                           const ShaderProgram& program,
                           const Pipeline& pipeline, PostProcessPass* out) noexcept {
    out->program = &program;
    out->pipeline = &pipeline;

    if (!AllocateSets(descriptors, program.setLayouts[kFrameSet], kFramesInFlight,
                      out->sets)) {
        return false;
    }

    // The pointer and the set that names it are written in the same step, so the two
    // cannot come to disagree about which image frame i reads.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        out->source[i] = source[i];
        const BindingValue values[] = {{source[i]->view.handle}};
        UpdateSet(descriptors, program.setLayouts[kFrameSet], out->sets[i], values, 1);
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
// Depth only, and every item in one go: what casts a shadow is a shape, so nothing
// here binds a material or sets a cull mode per draw.
static void RecordShadowPass(const FrameSlot& slot, const ShadowPass& shadow,
                             const DrawList& draws) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const ShadowPass::PerFrame& frame = shadow.frames[slot.index];
    const VkExtent2D extent = frame.depth.desc.extent;

    // oldLayout UNDEFINED: loadOp CLEAR overwrites, and the last frame's map is spent.
    // The image was left SHADER_READ_ONLY by the frame before, and discarding that is
    // exactly what UNDEFINED means.
    RecordLayoutTransition(vk, cmd, frame.depth.image.handle, VK_IMAGE_ASPECT_DEPTH_BIT,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                               | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    // storeOp STORE, unlike the scene pass's depth: this one is the product.
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = frame.depth.view.handle;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = 1.0f;   // nothing seen yet is farthest

    // colorAttachmentCount 0 and no pColorAttachments. The pipeline was compiled the
    // same way, from a fragment stage that declares no outputs.
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;

    vk.vkCmdBeginRendering(cmd, &rendering);

    // Depth is this pass's whole product, so both halves of it are on. Culling stays
    // off: it would be a choice about which face writes the depth, and unculled the
    // value is the nearest surface either way -- which is what the comparison wants.
    //
    // ViewportY::Down settles one thing here, the direction the map's v axis runs.
    // scene.frag reads it back as ndc * 0.5 + 0.5, which is this sign; the winding
    // rides along and has no effect on a pass that culls nothing.
    RasterState raster;
    raster.viewportY = ViewportY::Down;
    raster.depthTest = VK_TRUE;
    raster.depthWrite = VK_TRUE;
    SetRasterState(vk, cmd, VkRect2D{{0, 0}, extent}, raster);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow.pipeline->handle);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               shadow.program->layout, kFrameSet, 1, &frame.set,
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
        vk.vkCmdPushConstants(cmd, shadow.program->layout, VK_SHADER_STAGE_VERTEX_BIT,
                              0, sizeof(item.model), &item.model);
        vk.vkCmdDrawIndexed(cmd, item.range.count, 1, item.range.firstIndex,
                            item.vertexOffset, 0);
    }

    vk.vkCmdEndRendering(cmd);

    // Handed over here rather than at the top of the scene pass. The pass that wrote
    // an image is what knows when it stopped writing, and this keeps the scene pass
    // from having to name a pass it only reads through a descriptor.
    RecordLayoutTransition(vk, cmd, frame.depth.image.handle, VK_IMAGE_ASPECT_DEPTH_BIT,
                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// What the panel decided about rasterization, as the three values this pass uses.
// Gathered into one struct so the signature does not grow a parameter per checkbox.
struct SceneRasterOptions {
    bool wireframe = false;
    bool depthTest = true;
    bool depthWrite = true;
    bool rasterizerDiscard = false;
    VkCullModeFlags cull = kCullFromMaterial;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
};

static void RecordScenePass(const FrameSlot& slot, const ScenePass& scene,
                            const DrawList& draws, SceneRasterOptions raster,
                            DrawStats* stats) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const Mesh& mesh = *scene.mesh;

    // One choice for the whole pass. Both were built from scene.program, so every set
    // allocated for this pass fits either one and nothing below changes.
    const Pipeline& pipeline = raster.wireframe ? *scene.wirePipeline : *scene.pipeline;

    // Sets and push constants go through the pass's layout, not the pipeline's: every
    // pipeline a draw here can name was built from the same program, so this is the
    // one thing that stays put when the bound pipeline changes.
    const VkPipelineLayout layout = scene.program->layout;

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
    color.imageView = targets.color.view.handle;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = targets.colorResolve.view.handle;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.clearValue.color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

    // Clear 1.0 = farthest, paired with the pipeline's compareOp=LESS.
    // DONT_CARE: depth is used only within this frame.
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = targets.depth.view.handle;
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

    // Up, because our world is y-up, and the pass is where that belongs: every draw in
    // here shares one viewport, and no pipeline had to be compiled knowing it.
    //
    // Five of these come from the panel. None of them is compiled in, so the whole
    // set costs one call per pass -- which is the difference between this and the
    // wireframe switch beside them, where polygonMode forced a second pipeline.
    //
    // cull is the starting value; the loop below changes it per draw unless the panel
    // overrode it.
    RasterState state;
    state.viewportY = ViewportY::Up;
    state.cull = raster.cull == kCullFromMaterial ? VK_CULL_MODE_NONE : raster.cull;
    state.depthTest = raster.depthTest ? VK_TRUE : VK_FALSE;
    state.depthWrite = raster.depthWrite ? VK_TRUE : VK_FALSE;
    state.depthCompare = raster.depthCompare;
    state.rasterizerDiscard = raster.rasterizerDiscard ? VK_TRUE : VK_FALSE;
    SetRasterState(vk, cmd, VkRect2D{{0, 0}, extent}, state);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);

    // Once, above the loop: it is this frame's, and every draw in the pass reads it.
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
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
// Output: the largest rect inside dest that has source's aspect, centred
//
// The 2D -> 2D contract in Pipeline.h, held for the one pass that owes it. Fitting by
// whichever side runs out first is what "largest that still fits" means, and the
// leftover split in two is what centres it.
//
// Equal aspects give back {{0, 0}, dest} exactly, which is what every window at the
// render target's own shape gets -- the case this has to leave alone.
//
// Integer throughout, and the comparison is cross-multiplied rather than two
// divisions: same answer, and no float to round the wrong way at the boundary.
static VkRect2D LetterboxInto(VkExtent2D source, VkExtent2D dest) noexcept {
    const uint64_t sourceIsWider = uint64_t{source.width} * dest.height;
    const uint64_t destIsWider = uint64_t{dest.width} * source.height;

    VkExtent2D fitted = dest;
    if (sourceIsWider > destIsWider) {
        // Width fills the target and the bars are above and below.
        fitted.height = static_cast<uint32_t>(uint64_t{dest.width} * source.height
                                              / source.width);
    } else if (sourceIsWider < destIsWider) {
        fitted.width = static_cast<uint32_t>(uint64_t{dest.height} * source.width
                                             / source.height);
    }

    VkRect2D area{};
    area.offset.x = static_cast<int32_t>((dest.width - fitted.width) / 2);
    area.offset.y = static_cast<int32_t>((dest.height - fitted.height) / 2);
    area.extent = fitted;
    return area;
}

static void RecordPostProcessPass(const FrameSlot& slot, const PostProcessPass& post,
                                  const Texture& target) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;
    VkCommandBuffer cmd = slot.cmd;
    const Pipeline& pipeline = *post.pipeline;
    const VkPipelineLayout layout = post.program->layout;

    // Handed in rather than found: this pass does not know what drew it. The set
    // bound below names this same image, both picked by slot.index.
    const Texture& source = *post.source[slot.index];
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
    swapColor.imageView = dest.view.handle;
    swapColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // CLEAR, where it used to be DONT_CARE because the draw covered everything. It
    // does not any more: a window whose shape differs from the source's leaves bars,
    // and this is what is in them. renderArea below is still the whole target, so the
    // clear reaches them -- a clear follows the render area and not the viewport.
    //
    // Unconditional, so a window at the source's own shape pays a clear it does not
    // need. Making it conditional would put the same decision in two places, and this
    // is a full-screen write the driver does with the fast path.
    swapColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    swapColor.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    swapColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = destExtent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &swapColor;

    vk.vkCmdBeginRendering(cmd, &rendering);

    // Down, the opposite of the scene pass: fullscreen.vert builds its own uv from
    // gl_VertexIndex and expects the default orientation. One triangle, wound to face
    // us, and nothing to hide behind anything -- so everything else is the default.
    //
    // The area is the whole point. fullscreen.vert's uv runs 0..1 over the source no
    // matter what, so the shape of the picture is decided here and nowhere else: hand
    // in the whole target and it stretches. This is the only call of the four that
    // passes anything but the target it draws on.
    RasterState raster;
    raster.cull = VK_CULL_MODE_BACK_BIT;
    SetRasterState(vk, cmd, LetterboxInto(source.desc.extent, destExtent), raster);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);

    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                               0, 1, &post.sets[slot.index], 0, nullptr);

    // 3 vertices, no buffer. The shader builds them from gl_VertexIndex.
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRendering(cmd);

    // The target is left COLOR_ATTACHMENT_OPTIMAL, which is this pass's whole output
    // contract. What happens to it next -- another pass on top, or the screen -- is
    // not this function's to know, and the transition that used to be here said
    // otherwise.
}

bool RecordFrame(const FrameSlot& slot, const FrameLight* lights,
                 const ShadowPass& shadow, const ScenePass& scene,
                 const PostProcessPass& post, Gui& gui, const Texture& target,
                 const DrawList& draws, DrawStats* stats) noexcept {
    const VolkDeviceTable& vk = slot.dev->table;

    // The value and its GPU copy meet here. Safe because BeginFrame waited on this
    // slot's fence, and this runs after it -- an acquired image is its precondition.
    // Every uniform a frame writes, in the order the passes read them. The panel's
    // switches are among them even though the gui pass runs last: what reads them is
    // the scene pass, two passes earlier in the same submission.
    UploadGuiOptions(gui, slot.index);
    const FrameLight& light = lights[slot.index];
    std::memcpy(light.buffer.mapped, &light.value, sizeof(light.value));
    const ScenePass::PerFrame& frame = scene.frames[slot.index];
    std::memcpy(frame.cameraUniform.mapped, &frame.cameraValue,
                sizeof(frame.cameraValue));
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

    // The order is here, in these lines, and nowhere else. Both dependencies are
    // written somewhere -- the scene's set names the shadow map, post.source names the
    // images the scene resolves into -- and neither says anything about when. A set
    // naming a map cannot say the map was drawn this frame; that is what these lines
    // say, by being in this order.
    RecordShadowPass(slot, shadow, draws);
    RecordScenePass(slot, scene, draws,
                    SceneRasterOptions{GuiWireframe(gui), GuiDepthTest(gui),
                                       GuiDepthWrite(gui), GuiRasterizerDiscard(gui),
                                       GuiCullMode(gui), GuiDepthCompare(gui)},
                    stats);
    RecordPostProcessPass(slot, post, target);
    RecordGuiPass(slot, gui, target);

    // The frame leaves for the presentation engine here, after everything that draws
    // into it. This used to sit at the end of the post-process pass, which made that
    // pass assume its target was a swapchain image -- adding a second pass on top is
    // what forced it out.
    //
    // dstAccess is 0, unlike every other barrier here: present is not a queue
    // operation and reads nothing through the memory model, so there is no access to
    // make visible. The semaphore SubmitFrame signals is what present actually waits
    // on -- this barrier only has to leave the image in the right layout.
    RecordLayoutTransition(vk, cmd, target.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    if (vk.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG("[vk] vkEndCommandBuffer failed\n");
        return false;
    }
    return true;
}
