#include "Vulkan/Pipeline.h"

#include "Vulkan/Shader.h"

#include <iterator>   // std::size

// Negating height alone puts the image off screen: the origin has to move down by
// the same amount. The two lines are one thing.
VkViewport MakeViewport(VkExtent2D extent, ViewportY y) noexcept {
    const float width = static_cast<float>(extent.width);
    const float height = static_cast<float>(extent.height);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = (y == ViewportY::Up) ? height : 0.0f;
    viewport.width = width;
    viewport.height = (y == ViewportY::Up) ? -height : height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    return viewport;
}

// Effect: checks what the shaders declare against the one thing they cannot know --
//         the vertex layout's offsets, which live in Vertex.
//
// Push ranges and set layouts are no longer compared: they are built from the same
// reflection, so there are no two sides left to disagree.
static bool CheckVertexInterface(const GraphicsPipelineDesc& desc,
                                 const ShaderInterface& vs,
                                 const char* vertPath) noexcept {
    const uint32_t declared = desc.vertexInput != nullptr
                            ? desc.vertexInput->vertexAttributeDescriptionCount : 0;
    if (vs.inputCount != declared) {
        LOG("[vk] %s reads %u vertex inputs, the pipeline declares %u\n",
            vertPath, vs.inputCount, declared);
        return false;
    }
    // A gap means a location is declared but never read. The layer catches it later;
    // catching it here names the shader.
    if (vs.inputCount != vs.maxInputLocation) {
        LOG("[vk] %s has gaps in its input locations (%u inputs, highest is %u)\n",
            vertPath, vs.inputCount, vs.maxInputLocation);
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
                                   const ShaderProgram& program,
                                   const GraphicsPipelineDesc& desc,
                                   Pipeline* out) noexcept {
    Pipeline& pipeline = *out;
    pipeline.dev = &dev;
    pipeline.program = &program;
    pipeline.desc = desc;   // what it was built from, for recording and for a rebuild

    // The shaders, their set layouts and their pipeline layout are the program's --
    // several pipelines share one. Only the state below is this variant's.
    //
    // A rebuild reaches here with all of that already made, which is why the rebuild
    // does not strand the sets allocated from those layouts.
    if (!CheckVertexInterface(desc, program.vertInterface, program.vertPath)) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = program.vert;
    stages[0].pName = "main";           // entry point name
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = program.frag;
    stages[1].pName = "main";

    // --- Input: how bytes become vertices, vertices become primitives -------
    // Layout comes from desc. The topology is fixed for both.

    // Used when desc gives none, which means no vertex buffer at all.
    const VkPipelineVertexInputStateCreateInfo emptyVertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // 3 vertices = 1 triangle

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
    constexpr VkDynamicState kDynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_CULL_MODE,   // core in Vulkan 1.3, which we require
    };
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(std::size(kDynamicStates));
    dynamicState.pDynamicStates = kDynamicStates;

    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = desc.polygonMode;
    // Ignored: VK_DYNAMIC_STATE_CULL_MODE is in the list above.
    // Derived, so it cannot disagree with the viewport sign (Pipeline.h).
    rasterization.frontFace = FrontFaceFor(desc.viewportY);
    rasterization.lineWidth = 1.0f;   // used by LINE only. Above 1.0 needs wideLines

    // --- Fragment output: samples, blending, depth --------------------------
    // desc.formats.samples and desc.blending decide; every other value is fixed for both.

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    // sampleShadingEnable stays off: the aliasing we see is on edges, which the
    // rasterizer already handles. Shimmering textures would make the case for it.
    multisample.rasterizationSamples = desc.formats.samples;

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

    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    // depthFormat decides whether this state exists at all, further down.
    const bool useDepth = desc.formats.depth != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;   // translucent behind opaque is still hidden
    depthStencil.depthWriteEnable = translucent ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;   // clear is 1.0, so nearer wins

    // --- What it draws into: attachment formats -----------------------------
    // Dynamic rendering writes the formats here instead of into a VkRenderPass.
    // This is the exact point where a pipeline becomes tied to a render target.

    VkPipelineRenderingCreateInfo pipelineRendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pipelineRendering.colorAttachmentCount = 1;
    pipelineRendering.pColorAttachmentFormats = &desc.formats.color;
    pipelineRendering.depthAttachmentFormat = desc.formats.depth;   // UNDEFINED = no depth

    // --- Assemble and compile -----------------------------------------------

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &pipelineRendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState =
        desc.vertexInput != nullptr ? desc.vertexInput : &emptyVertexInput;
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
        LOG("[vk] vkCreateGraphicsPipelines failed (%d): %s\n", created, program.vertPath);
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
