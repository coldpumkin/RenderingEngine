#include "Vulkan/Pipeline.h"

// The vertex layout has to be described here.
#include "Vulkan/Buffer.h"

#include <cstdio>
#include <vector>

// Input:  path (Shaders/ next to the executable - CMake drops them there)
// Output: VkShaderModule (VK_NULL_HANDLE on failure)
static VkShaderModule LoadShader(const VulkanDevice& dev, const char* path) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        LOG("[vk] cannot open shader: %s\n", path);
        return VK_NULL_HANDLE;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    // SPIR-V is an array of 32-bit words. Not a multiple of 4 means a broken file.
    if (size <= 0 || (size % 4) != 0) {
        LOG("[vk] bad SPIR-V size %ld: %s\n", size, path);
        std::fclose(file);
        return VK_NULL_HANDLE;
    }

    // Read into a uint32_t vector because pCode requires 4-byte alignment. A char
    // array plus a cast does not guarantee it.
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    const size_t read = std::fread(code.data(), 1, static_cast<size_t>(size), file);
    std::fclose(file);
    if (read != static_cast<size_t>(size)) {
        LOG("[vk] short read: %s\n", path);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = static_cast<size_t>(size);   // bytes, not words
    info.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (dev.table.vkCreateShaderModule(dev.handle, &info, nullptr, &module) != VK_SUCCESS) {
        LOG("[vk] vkCreateShaderModule failed: %s\n", path);
        return VK_NULL_HANDLE;
    }
    return module;
}

// What actually differs between our two pipelines
// ============================================================================
//
// Anything not in this struct is identical in both and lives in
// CreateGraphicsPipeline once. A field is here because a shared constant would
// be wrong for one of the two passes.
//
// The fields group by who decides them, and that grouping is what the
// Create*Pipeline signatures below expose:
//
//   the shader decides   vertPath . fragPath . vertexInput . pushConstants
//                        viewportY . cullMode
//   the target decides   colorFormat . depthFormat . samples
//   the caller decides   polygonMode . blending
//
// setLayout is a fourth case: the shader decides its shape but Descriptors owns
// the object, so it is passed through rather than decided anywhere here.
struct GraphicsPipelineDesc {
    const char* vertPath = nullptr;
    const char* fragPath = nullptr;

    // nullptr means no vertex buffer - the shader builds its points from
    // gl_VertexIndex.
    const VkPipelineVertexInputStateCreateInfo* vertexInput = nullptr;

    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    // UNDEFINED means no depth attachment and no depth test. A separate bool
    // would make "format given, test off" expressible.
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    // Must equal the sample count of the attachments. A mismatch is caught at
    // vkCmdBeginRendering.
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    // What the shader receives besides vertices. Either, both or neither.
    const VkPushConstantRange* pushConstants = nullptr;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;

    // frontFace is derived from this (Pipeline.h), and pairs with the viewport
    // on the recording side.
    ViewportY viewportY = ViewportY::Down;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;

    // LINE requires the device's fillModeNonSolid (Core.h). Without it this
    // pipeline fails to build.
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

    // blendEnable and depthWriteEnable are both derived from this (Pipeline.h).
    Blending blending = Blending::Opaque;
};

// Input:  extent, viewport y direction
// Output: a viewport with the sign applied
//
// Flipping y by negating height alone puts the image off screen: the origin has
// to move down by the same amount. The two lines are one thing, which is why
// they live in a function together.
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

// Everything both pipelines share, in the order the GPU walks it
// ============================================================================
//
// vertices in -> primitives -> raster -> fragments out, then the two interfaces:
// what the shader may reach, and what it draws into. The blocks below follow
// that order, and each one says where its values come from.
//
// Almost all of it is baked at creation time. That is the point of a Vulkan
// pipeline - the driver compiles it ahead of the draw - and pDynamicState is the
// per-item escape hatch. Only two items take it here.
static bool CreateGraphicsPipeline(const VulkanDevice& dev,
                                   const GraphicsPipelineDesc& desc,
                                   Pipeline* out) noexcept {
    Pipeline& pipeline = *out;
    pipeline.dev = &dev;
    // The recording side reads this to build its viewport. Same source as the
    // frontFace below.
    pipeline.viewportY = desc.viewportY;

    // --- Shaders: desc.vertPath, desc.fragPath ------------------------------

    VkShaderModule vs = LoadShader(dev, desc.vertPath);
    VkShaderModule fs = LoadShader(dev, desc.fragPath);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        if (vs != VK_NULL_HANDLE) { dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr); }
        if (fs != VK_NULL_HANDLE) { dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr); }
        return false;
    }

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

    // The values stay out of the pipeline. Baked here, every resize would need a
    // rebuild.
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
    // Derived, never written by hand: the other half of this pair is the viewport
    // sign on the recording side (Pipeline.h).
    rasterization.frontFace = FrontFaceFor(desc.viewportY);
    rasterization.lineWidth = 1.0f;   // used by LINE only. Above 1.0 needs wideLines

    // --- Fragment output: samples, blending, depth --------------------------
    // desc.samples and desc.blending decide; every other value is fixed for both.

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    // sampleShadingEnable stays off. It would run the shader per sample, and the
    // aliasing we can see is on triangle edges, which the rasterizer already
    // handles. Shimmering textures are what would make a case for it.
    multisample.rasterizationSamples = desc.samples;

    // One value, two states. Keeping them apart would let them disagree.
    const bool translucent = desc.blending == Blending::Translucent;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                   | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = translucent ? VK_TRUE : VK_FALSE;
    // Straight alpha: src*a + dst*(1-a).
    //
    // The attachment is sRGB, so this multiply happens in linear space - the
    // hardware decodes dst, blends, and encodes again. The result differing from
    // a halfway mix of the stored bytes is correct.
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
    const bool useDepth = desc.depthFormat != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;   // translucent behind opaque is still hidden
    // Derived, never written by hand: paired with blendEnable above (Pipeline.h).
    depthStencil.depthWriteEnable = translucent ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;   // clear is 1.0, so nearer wins

    // --- What the shader may reach: push constants and descriptor sets ------

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (desc.pushConstants != nullptr) {
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = desc.pushConstants;
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
    pipelineRendering.pColorAttachmentFormats = &desc.colorFormat;
    pipelineRendering.depthAttachmentFormat = desc.depthFormat;   // UNDEFINED = no depth

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

// Scene pass
bool CreateTrianglePipeline(const VulkanDevice& dev,
                            RenderTargetFormats formats,
                            VkDescriptorSetLayout setLayout,
                            VkPolygonMode polygonMode,
                            Blending blending,
                            Pipeline* out) noexcept {
    // The vertex layout, which is what triangle.vert requires:
    //   binding     one buffer slot. stride is the size of one vertex
    //   attribute   one field inside it. location pairs with layout(location=N) in
    //
    // The format decides the size too - a vec2 here feeds a vec3 in the shader
    // with z = 0. It fails silently, so offsetof ties the offsets to the struct.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0].location = 0;                             // layout(location = 0) in vec3
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);
    attributes[1].location = 1;                             // layout(location = 1) in vec3
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, color);
    attributes[2].location = 2;                             // layout(location = 2) in vec2
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;         // vec2, unlike the two above
    attributes[2].offset = offsetof(Vertex, uv);

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(std::size(attributes));
    vertexInput.pVertexAttributeDescriptions = attributes;

    // stageFlags must cover every stage that reads the block. Both do: the vertex
    // shader reads mvp, the fragment shader reads alpha. A missing stage is
    // caught by the validation layer.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    GraphicsPipelineDesc desc;
    desc.vertPath = "Shaders/triangle.vert.spv";
    desc.fragPath = "Shaders/triangle.frag.spv";
    desc.vertexInput = &vertexInput;
    desc.colorFormat = formats.color;
    desc.depthFormat = formats.depth;
    desc.samples = formats.samples;
    desc.pushConstants = &pushRange;
    // sceneLayout, passed in. It differs from the present one because
    // triangle.frag reads two sampler2D (Descriptors.h).
    desc.setLayout = setLayout;
    // Our world coordinates are y-up, so the viewport flips. frontFace follows
    // from this (Pipeline.h).
    desc.viewportY = ViewportY::Up;
    desc.cullMode = VK_CULL_MODE_BACK_BIT;
    desc.polygonMode = polygonMode;
    desc.blending = blending;

    if (!CreateGraphicsPipeline(dev, desc, out)) { return false; }
    LOG("[vk] triangle pipeline ready (polygonMode=%d blending=%d)\n",
        static_cast<int>(polygonMode), static_cast<int>(blending));
    return true;
}

// Present pass. No vertices and no depth; it reads an image instead.
bool CreateFullscreenPipeline(const VulkanDevice& dev,
                              VkFormat colorFormat,
                              VkDescriptorSetLayout setLayout,
                              Pipeline* out) noexcept {
    GraphicsPipelineDesc desc;
    desc.vertPath = "Shaders/fullscreen.vert.spv";
    desc.fragPath = "Shaders/fullscreen.frag.spv";
    // no vertexInput   - the shader builds three points from gl_VertexIndex
    // no depthFormat   - depth comparison means nothing for a screen-covering triangle
    // no samples       - the swapchain image is handed to us, and it is 1-sample.
    //                    MSAA already ended in the scene pass resolve
    desc.colorFormat = colorFormat;   // the swapchain's - a different thing to match
    desc.setLayout = setLayout;
    // No flip here: the shader makes its own uv, and flipping would turn the
    // image upside down. That is why frontFace comes out opposite to the scene's.
    //
    // Culling is not for performance with a single triangle. It is on so that a
    // winding that breaks the convention blacks the screen out immediately -
    // without it the derivation above would be a value nobody could ever check.
    desc.viewportY = ViewportY::Down;
    desc.cullMode = VK_CULL_MODE_BACK_BIT;

    if (!CreateGraphicsPipeline(dev, desc, out)) { return false; }
    LOG("[vk] fullscreen pipeline ready\n");
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
