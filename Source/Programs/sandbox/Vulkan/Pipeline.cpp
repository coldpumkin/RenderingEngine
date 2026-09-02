#include "Vulkan/Pipeline.h"

#include "Vulkan/Shader.h"

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
                                 const ShaderInterface& vs) noexcept {
    const uint32_t declared = desc.vertexInput != nullptr
                            ? desc.vertexInput->vertexAttributeDescriptionCount : 0;
    if (vs.inputCount != declared) {
        LOG("[vk] %s reads %u vertex inputs, the pipeline declares %u\n",
            desc.vertPath, vs.inputCount, declared);
        return false;
    }
    // A gap means a location is declared but never read. The layer catches it later;
    // catching it here names the shader.
    if (vs.inputCount != vs.maxInputLocation) {
        LOG("[vk] %s has gaps in its input locations (%u inputs, highest is %u)\n",
            desc.vertPath, vs.inputCount, vs.maxInputLocation);
        return false;
    }
    return true;
}

// The vertex layout, derived entirely from Vertex: stride, offsets and formats all
// come from the struct, so a field change cannot desync them. A second vertex type
// gets its own pair beside this one.
//
// A narrower format fills the rest silently - a vec2 here feeds a vec3 with z = 0.
// One entry per attribute the shader reads; the layer warns about any extra.
const VkPipelineVertexInputStateCreateInfo& VertexInput() noexcept {
    static constexpr VkVertexInputBindingDescription binding{
        0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};

    static constexpr VkVertexInputAttributeDescription attributes[]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, uv)},
    };

    static const VkPipelineVertexInputStateCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
        1, &binding,
        static_cast<uint32_t>(std::size(attributes)), attributes};
    return info;
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
    // The recording side reads this to build its viewport.
    pipeline.viewportY = desc.viewportY;

    // --- Shaders: desc.vertPath, desc.fragPath ------------------------------

    ShaderInterface vsIface;
    ShaderInterface fsIface;
    VkShaderModule vs = LoadShader(dev, desc.vertPath, &vsIface);
    VkShaderModule fs = LoadShader(dev, desc.fragPath, &fsIface);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        if (vs != VK_NULL_HANDLE) { dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr); }
        if (fs != VK_NULL_HANDLE) { dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr); }
        return false;
    }

    if (!CheckVertexInterface(desc, vsIface)) {
        dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr);
        dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr);
        return false;
    }

    // One block, however many stages read it. Each stage reports only itself, so the
    // flags are or-ed and the size is whichever declared one -- they are the same
    // block, and glslc rejects a disagreement inside the shaders.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = vsIface.pushStages | fsIface.pushStages;
    pushRange.size = vsIface.pushSize > fsIface.pushSize ? vsIface.pushSize
                                                         : fsIface.pushSize;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";           // entry point name
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
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

    constexpr VkDynamicState kDynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(std::size(kDynamicStates));
    dynamicState.pDynamicStates = kDynamicStates;

    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = desc.polygonMode;
    rasterization.cullMode = desc.cullMode;
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

    // --- What the shader may reach: push constants and descriptor sets ------

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (pushRange.size != 0) {
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
    }
    if (desc.setLayout != VK_NULL_HANDLE) {
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &desc.setLayout;
    }
    // A layout is required even when both are empty.
    if (dev.table.vkCreatePipelineLayout(dev.handle, &layoutInfo, nullptr, &pipeline.layout)
            != VK_SUCCESS) {
        LOG("[vk] vkCreatePipelineLayout failed: %s\n", desc.vertPath);
        dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr);
        dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr);
        return false;
    }

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
    info.layout = pipeline.layout;
    info.renderPass = VK_NULL_HANDLE;   // none: dynamic rendering

    const VkResult created = dev.table.vkCreateGraphicsPipelines(
        dev.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.handle);

    // The modules are done once the pipeline exists - the code was compiled into it.
    dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr);
    dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr);

    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateGraphicsPipelines failed (%d): %s\n", created, desc.vertPath);
        return false;   // ~Pipeline cleans up the layout
    }
    return true;
}

void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept {
    if (pipeline->handle != VK_NULL_HANDLE) {
        dev.table.vkDestroyPipeline(dev.handle, pipeline->handle, nullptr);
    }
    if (pipeline->layout != VK_NULL_HANDLE) {
        dev.table.vkDestroyPipelineLayout(dev.handle, pipeline->layout, nullptr);
    }
    // dev survives: Create* overwrites it anyway, and clearing it here would
    // leave the destructor with nothing to free if a rebuild failed.
    pipeline->handle = VK_NULL_HANDLE;
    pipeline->layout = VK_NULL_HANDLE;
}

Pipeline::~Pipeline() {
    if (dev == nullptr) { return; }
    DestroyPipeline(*dev, this);
}
