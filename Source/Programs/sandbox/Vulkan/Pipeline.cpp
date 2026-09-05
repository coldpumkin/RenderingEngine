#include "Vulkan/Pipeline.h"

#include "Vulkan/Shader.h"


// Negating height alone puts the image off screen: the origin has to move down by
// the same amount. The two lines are one thing, and with an offset the amount is
// measured from the bottom of the area rather than from the bottom of the target.
VkViewport MakeViewport(VkRect2D area, ViewportY y) noexcept {
    const float left = static_cast<float>(area.offset.x);
    const float top = static_cast<float>(area.offset.y);
    const float width = static_cast<float>(area.extent.width);
    const float height = static_cast<float>(area.extent.height);

    VkViewport viewport{};
    viewport.x = left;
    viewport.y = (y == ViewportY::Up) ? top + height : top;
    viewport.width = width;
    viewport.height = (y == ViewportY::Up) ? -height : height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    return viewport;
}

// Walked from the pipeline's own list, so what was declared at creation and what is
// issued here cannot drift apart.
static void SetRasterState(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                           const VkDynamicState states[], uint32_t count,
                           VkRect2D area, const RasterState& raster) noexcept {
    for (uint32_t i = 0; i < count; ++i) {
        switch (states[i]) {
            case VK_DYNAMIC_STATE_VIEWPORT: {
                const VkViewport viewport = MakeViewport(area, raster.viewportY);
                vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
                break;
            }
            // The same rect as the viewport, which already confines the primitives
            // to it -- so this discards nothing until the two are told to differ.
            case VK_DYNAMIC_STATE_SCISSOR:
                vk.vkCmdSetScissor(cmd, 0, 1, &area);
                break;
            case VK_DYNAMIC_STATE_FRONT_FACE:
                vk.vkCmdSetFrontFace(cmd, FrontFaceFor(raster.viewportY));
                break;
            case VK_DYNAMIC_STATE_CULL_MODE:
                vk.vkCmdSetCullMode(cmd, raster.cull);
                break;
            case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
                vk.vkCmdSetDepthTestEnable(cmd, raster.depthTest);
                break;
            case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
                vk.vkCmdSetDepthWriteEnable(cmd, raster.depthWrite);
                break;
            case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
                vk.vkCmdSetDepthCompareOp(cmd, raster.depthCompare);
                break;
            case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:
                vk.vkCmdSetRasterizerDiscardEnable(cmd, raster.rasterizerDiscard);
                break;
            default:
                // Declared with nothing here to fill it. Vulkan says so at the first
                // draw; this names which state.
                LOG("[vk] dynamic state %d is declared and never set\n",
                    static_cast<int>(states[i]));
                break;
        }
    }
}

void BindPipeline(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                  const Pipeline& pipeline, VkRect2D area) noexcept {
    BindPipeline(vk, cmd, pipeline, area, pipeline.raster);
}

void BindPipeline(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                  const Pipeline& pipeline, VkRect2D area,
                  const RasterState& instead) noexcept {
    // Either order is legal: both are read at the draw, not here.
    SetRasterState(vk, cmd, pipeline.dynamicStates, pipeline.dynamicCount, area, instead);
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);
}

// Effect: checks what the shaders declare against what the resource actually supplies.
//
// The layout belongs to the vertex buffer, not to the shader: its stride, offsets and
// formats are the buffer's own, and one shader can be fed by several. So this is not
// an equality test -- it asks whether each location exists on both sides and delivers
// the kind of number the shader reads.
//
// Push ranges and set layouts are not compared: they are built from the same
// reflection, so there are no two sides left to disagree.
static bool CheckVertexInterface(const GraphicsPipelineDesc& desc,
                                 const ShaderInterface& vs,
                                 const char* vertPath) noexcept {
    const VertexLayout& layout = desc.vertexLayout;

    // Walked from the shader's side, and the direction is the point. The layout
    // describes the buffer, which is one thing; the shaders reading it are several,
    // and each reads the locations it needs. scene.vert takes all four of ours,
    // shadow.vert takes position, and both are built from the same layout.
    //
    // So a layout supplying more than this shader reads is not an error -- Vulkan
    // ignores the surplus. What it may not do is leave out something the shader reads,
    // or supply it as the wrong kind of number.
    for (uint32_t i = 0; i < vs.inputCount; ++i) {
        const InterfaceSlot& slot = vs.inputs[i];

        const VertexAttribute* attribute = nullptr;
        for (uint32_t j = 0; j < layout.attributeCount; ++j) {
            if (layout.attributes[j].location == slot.location) {
                attribute = &layout.attributes[j];
                break;
            }
        }
        if (attribute == nullptr) {
            LOG("[vk] %s reads location %u, which the layout does not supply\n",
                vertPath, slot.location);
            return false;
        }

        // The kind and not the count. Vulkan converts inside a kind and not across one
        // -- R8G8B8A8_UNORM feeds a vec4, R32G32B32A32_UINT does not -- while channels
        // the format does not supply are defined to arrive as (0, 0, 0, 1). Nothing is
        // undefined here, so there is nothing to refuse. The output end differs, and
        // that asymmetry is the spec's.
        const NumericKind supplied = ChannelsOfFormat(attribute->format).kind;
        if (supplied == NumericKind::Unknown) {
            LOG("[vk] %s: location %u uses format %d, which ChannelsOfFormat does not know\n",
                vertPath, slot.location, attribute->format);
            return false;
        }
        if (supplied != slot.kind) {
            LOG("[vk] %s: location %u is supplied as %s and read as %s\n",
                vertPath, slot.location, KindName(supplied), KindName(slot.kind));
            return false;
        }
    }

    // The layout's own consistency, which is not about this shader: feeding one
    // location twice is wrong whoever reads it, and the second entry would silently
    // win.
    uint32_t seen = 0;
    for (uint32_t i = 0; i < layout.attributeCount; ++i) {
        const uint32_t bit = 1u << layout.attributes[i].location;
        if ((seen & bit) != 0) {
            LOG("[vk] %s: the layout feeds location %u twice\n",
                vertPath, layout.attributes[i].location);
            return false;
        }
        seen |= bit;
    }
    return true;
}

// The other end of the same boundary, checked the same way. Until this existed,
// colorAttachmentCount was written as 1 and no shader was ever asked how many it
// writes.
static bool CheckOutputInterface(const AttachmentFormats& formats,
                                 const ShaderInterface& fs,
                                 const char* fragPath) noexcept {
    // Zero is a real answer, not a missing one: a depth-only pass writes no colour and
    // its whole product is the depth image. So the question is not how many outputs
    // there are, it is whether the two sides agree -- a target with nothing to write
    // it is as wrong as an output with nowhere to go.
    if (fs.outputCount != formats.colorCount) {
        LOG("[vk] %s writes %u colour outputs and was given %u colour targets\n",
            fragPath, fs.outputCount, formats.colorCount);
        return false;
    }

    // Location by location, the way the vertex end does it. One colour format means
    // one attachment at location 0; a second would be indexed here rather than named,
    // which is the shape this loop is already in.
    for (uint32_t i = 0; i < fs.outputCount; ++i) {
        const InterfaceSlot& slot = fs.outputs[i];

        // Indexed by location, because that is what a shader writes to. Reflection
        // refuses a gap, which is what makes the index and the location the same
        // number here.
        const VkFormat target = formats.color[slot.location];
        const FormatChannels stored = ChannelsOfFormat(target);
        if (stored.kind == NumericKind::Unknown) {
            LOG("[vk] %s: the colour format %d is one ChannelsOfFormat does not know\n",
                fragPath, static_cast<int>(target));
            return false;
        }
        if (stored.kind != slot.kind) {
            LOG("[vk] %s: location %u writes %s into an attachment that stores %s\n",
                fragPath, slot.location, KindName(slot.kind), KindName(stored.kind));
            return false;
        }

        // The other half of what the same format states. A channel the fragment stage
        // does not write is undefined -- Vulkan defines a fill for a vertex input and
        // none for this -- so unlike the vertex end, the count is a refusal here.
        //
        // The number comes from the format for the same reason the kind does: it is one
        // declaration, and reading half of it while writing the other half as a literal
        // is how the two drift apart.
        if (stored.count != slot.componentCount) {
            LOG("[vk] %s: location %u writes %u components into an attachment of %u\n",
                fragPath, slot.location, slot.componentCount, stored.count);
            return false;
        }
    }

    // Neither colour nor depth is a pipeline that draws nowhere. Vulkan permits it --
    // it is how a shader that only writes storage images is built -- and we have no
    // such thing, so it is a mistake here.
    if (fs.outputCount == 0 && formats.depth == VK_FORMAT_UNDEFINED) {
        LOG("[vk] %s writes no colour, and no depth format was given either\n", fragPath);
        return false;
    }
    return true;
}


// Everything both pipelines share, in the order the GPU walks it
// ============================================================================
//
// vertices in -> primitives -> raster -> fragments out, then the two interfaces:
// what the shader may reach, and what it draws into.
//
// Nearly all of it is baked at creation - that is what a Vulkan pipeline is - and
// pDynamicState is the escape hatch. Two items take it here.
bool CreateGraphicsPipeline(const VulkanDevice& dev,
                            const GraphicsPipelineDesc& desc,
                            Pipeline* out) noexcept {
    Pipeline& pipeline = *out;
    pipeline.dev = &dev;

    if (desc.program == nullptr) {
        LOG("[vk] a pipeline desc with no program\n");
        return false;
    }
    const ShaderProgram& program = *desc.program;
    pipeline.program = desc.program;

    // The shaders, their set layouts and their pipeline layout are the program's --
    // several pipelines share one. Only the state below is this variant's.
    //
    // A rebuild reaches here with all of that already made, which is why the rebuild
    // does not strand the sets allocated from those layouts.
    // The descs handed in, reduced to what compiling actually needs. Everything below
    // reads this, and it is what the Pipeline keeps.
    const AttachmentFormats formats =
        AttachmentFormatsOf(desc.targets, kMaxColorTargets + 1);

    // What it was built from, kept. The target pointers are not: their projection is.
    pipeline.vertexLayout = desc.vertexLayout;
    pipeline.formats = formats;
    pipeline.polygonMode = desc.polygonMode;
    pipeline.blending = desc.blending;
    pipeline.raster = desc.raster;
    pipeline.dynamicCount = desc.dynamicCount;
    for (uint32_t i = 0; i < desc.dynamicCount; ++i) {
        pipeline.dynamicStates[i] = desc.dynamicStates[i];
    }

    // The two ends of the chain, which are the two stages with a CPU-side partner: a
    // vertex stage answers to a VertexLayout, a fragment stage to an AttachmentFormats.
    // Asked for by stage rather than by position -- a fragment stage is optional, and
    // its absence is what a depth-only program is.
    const ProgramStage* vertStage = program.Stage(VK_SHADER_STAGE_VERTEX_BIT);
    const ProgramStage* fragStage = program.Stage(VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vertStage == nullptr) {
        LOG("[vk] a graphics pipeline needs a vertex stage: %s\n", program.stages[0].path);
        return false;
    }

    // No fragment stage writes nothing, which is what CheckOutputInterface is already
    // built to compare against, and the answer an empty fragment main used to give.
    static const ShaderInterface kWritesNothing;
    if (!CheckVertexInterface(desc, vertStage->interface, vertStage->path)
            || !CheckOutputInterface(formats,
                                     fragStage != nullptr ? fragStage->interface
                                                          : kWritesNothing,
                                     fragStage != nullptr ? fragStage->path
                                                          : "no fragment stage")) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[kMaxStagesPerProgram]{};
    for (uint32_t i = 0; i < program.stageCount; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage =
            static_cast<VkShaderStageFlagBits>(program.stages[i].interface.stage);
        stages[i].module = program.stages[i].module;
        stages[i].pName = "main";       // entry point name
    }

    // --- Input: how bytes become vertices, vertices become primitives -------
    // Layout comes from desc. The topology is fixed for both.

    // Built here from the value, so nothing outlives this call. stride 0 leaves both
    // counts at zero, which is what "no vertex buffer" is in Vulkan's terms.
    const VertexLayout& layout = desc.vertexLayout;
    VkVertexInputBindingDescription binding{0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX};

    // What this stage reads, not everything the buffer holds. The layout describes one
    // buffer and several shaders read it: scene.vert takes all four of our attributes,
    // shadow.vert takes position. Declaring the rest here draws correctly and the
    // validation layer warns once per pipeline that the attribute is not consumed --
    // so the surplus is dropped rather than passed on.
    //
    // The same shape as the fragment end, where colorAttachmentCount comes from the
    // .spv rather than from a number written here.
    VkVertexInputAttributeDescription attributes[kMaxVertexAttributes]{};
    uint32_t attributeCount = 0;
    for (uint32_t i = 0; i < layout.attributeCount; ++i) {
        const VertexAttribute& supplied = layout.attributes[i];

        bool read = false;
        for (uint32_t j = 0; j < vertStage->interface.inputCount; ++j) {
            if (vertStage->interface.inputs[j].location == supplied.location) {
                read = true;
                break;
            }
        }
        if (!read) { continue; }

        attributes[attributeCount] = {supplied.location, 0, supplied.format,
                                      supplied.offset};
        attributeCount += 1;
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (layout.stride != 0) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = attributeCount;
        vertexInput.pVertexAttributeDescriptions = attributes;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    // Compiled in, unlike the depth state below. See the note in Pipeline.h: the
    // dynamic version of this only moves within a topology class.
    inputAssembly.topology = desc.topology;

    // --- Raster: where the primitive lands and which side faces us ----------
    // Counts are fixed, the two values are set at record time, the rest is desc.

    // Values stay out: baked here, every resize would need a rebuild.
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Three items are not baked.
    //
    // viewport/scissor because they follow the window, and baking them would rebuild
    // every pipeline on a resize.
    //
    // cullMode because the asset decides it per draw: glTF doubleSided is a material
    // property, and Sponza has both kinds. Baking it meant a second pipeline that
    // differed in one field -- two shader compiles for one register.
    //
    // The reason all three are cheap to leave out is the same: none of them changes
    // the machine code. They are register values the driver sets before the draw.
    // blending or the sample count would be a different answer -- those change what
    // the fragment shader compiles to, and leaving them dynamic makes the compiler
    // assume the worst.
    //
    // Contract: a dynamic state must be set before every draw with this pipeline.
    //           Vulkan does not remember one across a command buffer.
    // All of them core in Vulkan 1.3, which we require. Every one is a register the
    // hardware reads per draw, so making it dynamic costs nothing at compile time and
    // saves a pipeline per value.
    //
    if (desc.dynamicCount > kMaxDynamicStates) {
        LOG("[vk] a desc declaring %u dynamic states, and we hold %u\n",
            desc.dynamicCount, kMaxDynamicStates);
        return false;
    }

    // Which of these is dynamic and which is baked is not about how often a value
    // changes. viewport, cull, winding and the depth test are registers the hardware
    // reads per draw; polygonMode, blending, sample count and the attachment formats
    // change what the driver compiles, so those stay in the desc. The wireframe
    // pipeline exists because of that line and nothing else does.
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = desc.dynamicCount;
    dynamicState.pDynamicStates = desc.dynamicStates;

    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = desc.polygonMode;
    // The same values BindPipeline would issue: whether they are read here or ignored
    // in favour of a command is what dynamicStates says.
    rasterization.cullMode = desc.raster.cull;
    rasterization.frontFace = FrontFaceFor(desc.raster.viewportY);
    rasterization.rasterizerDiscardEnable = desc.raster.rasterizerDiscard;
    rasterization.lineWidth = 1.0f;   // used by LINE only. Above 1.0 needs wideLines

    // --- Fragment output: samples, blending, depth --------------------------
    // desc.formats.samples and desc.blending decide; every other value is fixed for both.

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    // sampleShadingEnable stays off: the aliasing we see is on edges, which the
    // rasterizer already handles. Shimmering textures would make the case for it.
    multisample.rasterizationSamples = formats.samples;

    // One value, two states. Keeping them apart would let them disagree.
    const bool translucent = desc.blending == Blending::Translucent;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                   | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = translucent ? VK_TRUE : VK_FALSE;
    // Straight alpha: src*a + dst*(1-a). The attachment is sRGB, so the hardware
    // decodes, blends in linear space, and encodes again - the result is not a
    // halfway mix of the stored bytes, and that is correct.
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    // The alpha channel goes unused: our final destination is opaque.
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    // One state per colour attachment, and Vulkan reads that many -- the same state
    // for each, because blending is the pipeline's answer rather than a target's.
    // Zero leaves pAttachments unread.
    VkPipelineColorBlendAttachmentState blendAttachments[kMaxColorTargets];
    for (uint32_t i = 0; i < formats.colorCount; ++i) {
        blendAttachments[i] = blendAttachment;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = formats.colorCount;
    colorBlend.pAttachments = blendAttachments;

    // depthFormat decides whether this state exists at all, further down.
    const bool useDepth = formats.depth != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // The desc's own, for the same reason as the raster ones above.
    depthStencil.depthTestEnable = desc.raster.depthTest;
    depthStencil.depthWriteEnable = desc.raster.depthWrite;
    depthStencil.depthCompareOp = desc.raster.depthCompare;

    // --- What it draws into: attachment formats -----------------------------
    // Dynamic rendering writes the formats here instead of into a VkRenderPass.
    // This is the exact point where a pipeline becomes tied to a render target.

    VkPipelineRenderingCreateInfo pipelineRendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    // CheckOutputInterface already made these the same number, which is why either
    // side can be read here.
    pipelineRendering.colorAttachmentCount = formats.colorCount;
    pipelineRendering.pColorAttachmentFormats =
        formats.colorCount != 0 ? formats.color : nullptr;
    pipelineRendering.depthAttachmentFormat = formats.depth;   // UNDEFINED = no depth

    // --- Assemble and compile -----------------------------------------------

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &pipelineRendering;
    info.stageCount = program.stageCount;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = useDepth ? &depthStencil : nullptr;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamicState;
    info.layout = program.layout;
    info.renderPass = VK_NULL_HANDLE;   // none: dynamic rendering

    const VkResult created = dev.table.vkCreateGraphicsPipelines(
        dev.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.handle);

    // The modules are not destroyed here any more: they are the program's, and a second
    // variant built from it still needs them.
    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateGraphicsPipelines failed (%d): %s\n", created, vertStage->path);
        return false;
    }
    return true;
}


// Only the compiled object. The layout it was built against is the program's, and
// outliving a rebuild is the point of that: sets drawn from those layouts stay valid.
void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept {
    if (pipeline->handle != VK_NULL_HANDLE) {
        dev.table.vkDestroyPipeline(dev.handle, pipeline->handle, nullptr);
    }
    // dev and program survive: Create* overwrites them anyway, and clearing them here
    // would leave the destructor with nothing to free if a rebuild failed.
    pipeline->handle = VK_NULL_HANDLE;
}

Pipeline::~Pipeline() {
    if (dev == nullptr) { return; }
    DestroyPipeline(*dev, this);
}
