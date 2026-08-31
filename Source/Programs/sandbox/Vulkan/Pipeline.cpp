#include "Vulkan/Pipeline.h"

// Vertex layout을 기술해야 한다.
#include "Vulkan/Buffer.h"

#include <cstdio>
#include <vector>

// Input:  path (실행 파일 옆 Shaders/. CMake가 빌드할 때 거기로 떨군다)
// Output: VkShaderModule (실패하면 VK_NULL_HANDLE)
static VkShaderModule LoadShader(const VulkanDevice& dev, const char* path) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        LOG("[vk] cannot open shader: %s\n", path);
        return VK_NULL_HANDLE;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    // SPIR-V는 32비트 word 배열이다. 4의 배수가 아니면 파일이 깨진 것이다.
    if (size <= 0 || (size % 4) != 0) {
        LOG("[vk] bad SPIR-V size %ld: %s\n", size, path);
        std::fclose(file);
        return VK_NULL_HANDLE;
    }

    // uint32_t vector로 읽는 이유: pCode가 4바이트 정렬을 요구한다. char 배열로 읽어
    // 캐스팅하면 정렬이 보장되지 않는다.
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    const size_t read = std::fread(code.data(), 1, static_cast<size_t>(size), file);
    std::fclose(file);
    if (read != static_cast<size_t>(size)) {
        LOG("[vk] short read: %s\n", path);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = static_cast<size_t>(size);   // byte 수다 (word 수가 아니다)
    info.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (dev.table.vkCreateShaderModule(dev.handle, &info, nullptr, &module) != VK_SUCCESS) {
        LOG("[vk] vkCreateShaderModule failed: %s\n", path);
        return VK_NULL_HANDLE;
    }
    return module;
}

// 두 pipeline이 실제로 무엇이 다른가
// ============================================================================
//
// Scene용과 present용을 일부러 복제해서 써본 뒤 diff를 재봤다. 주석·빈줄 빼고 123줄
// 중 74줄(60%)이 같았고 다른 것은 다섯뿐이었다:
//
//   shader 경로 · vertex input · depth · pipeline layout의 내용 · attachment format
//
// 나머지 차이는 전부 주석과 log 문구였다. inputAssembly · multisample · dynamic
// state는 값이 완전히 같아서 인자로 안 뺐다.
//
// **나머지 셋은 소비자가 늘 때마다 하나씩 올라왔다.** 전부 같은 모양이다 - 새 소비자가
// 그 값을 다르게 요구했고, 공유 상수로 두면 한쪽이 반드시 틀린다:
//
//   rasterization  두 pass의 viewport y 부호가 반대다. 공유 상수였을 때 present 쪽이
//                  틀려 있었고 cullMode가 NONE이라 아무 증상이 없었다
//   polygonMode    같은 정점을 면으로도 선으로도 그린다
//   blending       반투명은 blend를 켜고 depth write를 끈다 - 한 값에서 둘이 나온다
struct GraphicsPipelineDesc {
    const char* vertPath = nullptr;
    const char* fragPath = nullptr;

    // nullptr이면 정점 버퍼를 안 쓴다 (셰이더가 gl_VertexIndex로 만든다).
    const VkPipelineVertexInputStateCreateInfo* vertexInput = nullptr;

    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    // UNDEFINED면 depth attachment도 depth test도 없다. Bool을 따로 두면 "format은
    // 줬는데 test는 껐다" 같은 어긋난 조합이 생긴다.
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    // 셰이더가 정점 말고 무엇을 받나. 둘 다 없어도, 둘 다 있어도 된다.
    const VkPushConstantRange* pushConstants = nullptr;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;

    // frontFace를 여기서 유도한다 (Pipeline.h). 기록 쪽 viewport와 짝이다.
    ViewportY viewportY = ViewportY::Down;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;

    // LINE은 device의 fillModeNonSolid를 요구한다 (Core.h). 안 켜져 있으면
    // 이 pipeline 생성이 실패한다.
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

    // blendEnable과 depthWriteEnable을 여기서 유도한다 (Pipeline.h).
    Blending blending = Blending::Opaque;
};

// Input:  extent, viewport의 y 방향
// Output: 부호가 맞춰진 viewport
//
// y를 뒤집을 때 height만 음수로 만들면 화면 밖으로 나간다. 원점을 아래로 옮기는
// y까지 같이 움직여야 해서 두 줄이 한 몸이다 - 그래서 함수 안에 같이 있다.
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

// 위 다섯을 뺀 나머지 - 두 파이프라인이 똑같이 쓰는 것들이 여기 한 번만 있다.
static bool CreateGraphicsPipeline(const VulkanDevice& dev,
                                   const GraphicsPipelineDesc& desc,
                                   Pipeline* out) noexcept {
    Pipeline& pipeline = *out;
    pipeline.dev = &dev;
    // 기록 쪽이 이걸 읽어 viewport를 만든다. frontFace와 같은 값에서 나온다.
    pipeline.viewportY = desc.viewportY;

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
    stages[0].pName = "main";           // 진입점 함수 이름
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // 안 준 경우를 위한 빈 것 - vertex buffer를 안 쓴다는 뜻이다.
    const VkPipelineVertexInputStateCreateInfo emptyVertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // vertex 3개 = 삼각형 1개

    // Dynamic state로 둔다. 여기 값을 박으면 창 크기가 바뀔 때마다 재생성해야 한다.
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
    // **손으로 안 적는다.** 반대편이 기록 쪽 viewport라 값으로 두면 어긋난다.
    rasterization.frontFace = FrontFaceFor(desc.viewportY);
    // FILL일 때는 안 쓰이는 값이었는데 LINE pipeline이 생기면서 실제로 쓰인다.
    // 1.0을 넘기려면 device의 wideLines가 따로 필요하다.
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;   // MSAA 없음

    const bool translucent = desc.blending == Blending::Translucent;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                   | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = translucent ? VK_TRUE : VK_FALSE;
    // straight alpha. src*a + dst*(1-a).
    //
    // **attachment가 sRGB라 이 곱셈이 linear 공간에서 일어난다** - 하드웨어가 dst를
    // 풀어서 섞고 다시 sRGB로 저장한다. 저장된 바이트를 반씩 섞은 값과 다르게 나오는
    // 것이 정상이다.
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    // alpha 채널은 안 쓴다 - 최종 목적지가 불투명한 우리 render target이다.
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    // compareOp=LESS + clear 1.0: 새 픽셀의 깊이가 기존보다 작을 때만 통과한다.
    // 반투명을 그릴 때는 depthWriteEnable을 끄고 정렬해 그린다 - 그때 이 둘이 갈린다.
    const bool useDepth = desc.depthFormat != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;   // 반투명도 불투명 뒤에 있으면 가려진다
    // **손으로 안 적는다.** blend와 짝이라 따로 두면 어긋난 조합이 생긴다.
    depthStencil.depthWriteEnable = translucent ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Shader가 받는 외부 자원의 모양. 둘 다 없어도 layout은 만들어야 한다.
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (desc.pushConstants != nullptr) {
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = desc.pushConstants;
    }
    if (desc.setLayout != VK_NULL_HANDLE) {
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &desc.setLayout;
    }
    if (dev.table.vkCreatePipelineLayout(dev.handle, &layoutInfo, nullptr, &pipeline.layout)
            != VK_SUCCESS) {
        LOG("[vk] vkCreatePipelineLayout failed: %s\n", desc.vertPath);
        dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr);
        dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr);
        return false;
    }

    // Dynamic rendering: VkRenderPass 대신 여기에 format을 미리 적는다.
    // Pipeline이 render target format에 묶이는 지점이 정확히 여기다.
    VkPipelineRenderingCreateInfo pipelineRendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pipelineRendering.colorAttachmentCount = 1;
    pipelineRendering.pColorAttachmentFormats = &desc.colorFormat;
    pipelineRendering.depthAttachmentFormat = desc.depthFormat;   // UNDEFINED면 뎁스 없음

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
    info.renderPass = VK_NULL_HANDLE;   // 다이나믹 렌더링이라 없다

    const VkResult created = dev.table.vkCreateGraphicsPipelines(
        dev.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.handle);

    // Shader module은 pipeline이 만들어지면 필요 없다 - 코드가 안으로 컴파일돼 들어갔다.
    dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr);
    dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr);

    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateGraphicsPipelines failed (%d): %s\n", created, desc.vertPath);
        return false;   // layout은 ~Pipeline이 정리한다
    }
    return true;
}

// Scene pass용
bool CreateTrianglePipeline(const VulkanDevice& dev,
                            RenderTargetFormats formats,
                            VkDescriptorSetLayout setLayout,
                            VkPolygonMode polygonMode,
                            Blending blending,
                            Pipeline* out) noexcept {
    // binding   buffer slot 하나. stride는 한 vertex의 크기
    // attribute 그 안의 필드 하나. location은 shader의 layout(location=N) in과 짝
    //
    // format이 크기까지 정한다 - shader가 vec3으로 받아도 여기가 vec2면 z가 0이 된다.
    // 조용히 틀리는 자리라 offsetof로 묶어둔다.
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
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;         // vec2다 - 위 둘과 다르다
    attributes[2].offset = offsetof(Vertex, uv);

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(std::size(attributes));
    vertexInput.pVertexAttributeDescriptions = attributes;

    // stageFlags가 실제로 읽는 stage와 맞아야 한다. 빠뜨리면 validation layer가 잡는다.
    // alpha가 생기면서 fragment도 이 블록을 읽는다 - 둘 다 적어야 한다.
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
    desc.pushConstants = &pushRange;
    // texture가 들어오면서 scene pipeline에도 set이 붙었다. present pipeline과 **같은
    // setLayout을 쓴다** - 둘 다 "0번은 image+sampler 하나"라 모양이 같기 때문이다.
    desc.setLayout = setLayout;
    // world 좌표가 y-up이라 뒤집는다. frontFace는 여기서 유도된다 (Pipeline.h).
    desc.viewportY = ViewportY::Up;
    desc.cullMode = VK_CULL_MODE_BACK_BIT;
    desc.polygonMode = polygonMode;
    desc.blending = blending;

    if (!CreateGraphicsPipeline(dev, desc, out)) { return false; }
    LOG("[vk] triangle pipeline ready (polygonMode=%d blending=%d)\n",
        static_cast<int>(polygonMode), static_cast<int>(blending));
    return true;
}

// Present pass용. Vertex도 depth도 없고 대신 image를 읽는다.
bool CreateFullscreenPipeline(const VulkanDevice& dev,
                              VkFormat colorFormat,
                              VkDescriptorSetLayout setLayout,
                              Pipeline* out) noexcept {
    GraphicsPipelineDesc desc;
    desc.vertPath = "Shaders/fullscreen.vert.spv";
    desc.fragPath = "Shaders/fullscreen.frag.spv";
    // vertexInput 없음   - shader가 gl_VertexIndex로 세 점을 만든다
    // depthFormat 없음   - 화면을 덮는 삼각형에 깊이 비교는 의미가 없다
    desc.colorFormat = colorFormat;   // swapchain format이다 - 맞추는 상대가 다르다
    desc.setLayout = setLayout;
    // 여기는 안 뒤집는다 - shader가 uv를 직접 만들어 쓰므로 뒤집으면 화면이 상하로
    // 뒤집힌다. 그래서 frontFace가 scene과 반대로 유도된다.
    //
    // 삼각형이 하나뿐이라 culling이 성능을 위한 것은 아니다. shader가 내는 winding이
    // 규약을 벗어나면 화면이 검게 나와 즉시 드러나라고 켠다 - 안 켜면 위의 유도가
    // 맞았는지 틀렸는지 영원히 알 수 없는 값이 된다.
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
    // dev를 지우지 않는다 - 다시 만들 때 Create*가 어차피 덮어쓰고, 여기서 비우면
    // 재생성이 실패했을 때 소멸자가 아무것도 못 지운다.
    pipeline->handle = VK_NULL_HANDLE;
    pipeline->layout = VK_NULL_HANDLE;
}

Pipeline::~Pipeline() {
    if (dev == nullptr) { return; }
    DestroyPipeline(*dev, this);
}
