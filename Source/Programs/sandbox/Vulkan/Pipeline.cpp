#include "Vulkan/Pipeline.h"

// 정점 레이아웃(Vertex)을 기술해야 한다.
#include "Vulkan/Buffer.h"

#include <cstdio>
#include <vector>

// ============================================================================
// 7. 그래픽스 파이프라인 - **무엇으로 그리는가**
// ============================================================================
//
// 지금까지는 "어디에 그리는가"(스왑체인 이미지)만 있었다. 파이프라인은 그 반대편이다:
// 정점을 어떻게 화면 좌표로 바꾸고, 픽셀 색을 어떻게 정하는가.
//
// **파이프라인은 스왑체인 포맷에 묶인다.** 아래 pipelineRendering.pColorAttachmentFormats가
// 그 자리다 - 다이나믹 렌더링에서는 VkRenderPass 대신 여기에 포맷을 미리 적어둔다.
// 그래서 포맷이 바뀌면 파이프라인도 다시 만들어야 한다. **리사이즈는 포맷을 안 바꾸므로
// 지금은 재생성이 필요 없다** (크기는 동적 상태로 매 프레임 준다).

// SPIR-V 파일 하나를 읽어 VkShaderModule로.
//
// 실행 파일 옆 Shaders/에서 찾는다. CMake가 빌드할 때 거기로 떨군다.
VkShaderModule LoadShader(const VulkanDevice& dev, const char* path) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        LOG("[vk] cannot open shader: %s\n", path);
        return VK_NULL_HANDLE;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    // SPIR-V는 32비트 워드 배열이다. 크기가 4의 배수가 아니면 파일이 깨진 것이다.
    if (size <= 0 || (size % 4) != 0) {
        LOG("[vk] bad SPIR-V size %ld: %s\n", size, path);
        std::fclose(file);
        return VK_NULL_HANDLE;
    }

    // uint32_t 벡터로 읽는 이유: pCode가 const uint32_t*이고 **4바이트 정렬을 요구한다.**
    // char 배열로 읽어 캐스팅하면 정렬이 보장되지 않는다.
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    const size_t read = std::fread(code.data(), 1, static_cast<size_t>(size), file);
    std::fclose(file);
    if (read != static_cast<size_t>(size)) {
        LOG("[vk] short read: %s\n", path);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = static_cast<size_t>(size);   // **바이트 수**다 (워드 수가 아니다)
    info.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (dev.table.vkCreateShaderModule(dev.handle, &info, nullptr, &module) != VK_SUCCESS) {
        LOG("[vk] vkCreateShaderModule failed: %s\n", path);
        return VK_NULL_HANDLE;
    }
    return module;
}

// ============================================================================
// 두 파이프라인이 실제로 무엇이 다른가
// ============================================================================
//
// 삼각형용과 전체화면용을 **일부러 복제해서 써본 뒤** diff를 재봤다.
// 주석·빈줄 빼고 123줄 중 **74줄(60%)이 같았고**, 다른 것은 다섯뿐이었다:
//
//   셰이더 경로 · 정점 입력 · 뎁스 · 파이프라인 레이아웃의 내용 · 첨부 포맷
//
// 나머지 차이는 전부 주석과 로그 문구였다. inputAssembly·rasterization·multisample·
// 블렌딩·뷰포트·동적 상태는 값이 완전히 같았다 - **아직 달라질 이유가 없어서**
// 인자로 안 뺐다. MSAA를 켜거나 와이어프레임을 그리게 되면 그때 하나씩 올라온다.
//
// 복제해두지 않았으면 무엇이 진짜 공통인지 추측해야 했다. 예를 들어 "셰이더 스테이지
// 두 개"는 공통일 것 같지만 실제로는 경로만 다르고 나머지가 같았고, "정점 입력"은
// 파라미터화할 것 같았지만 전체화면 쪽은 **아예 없는** 것이라 bool로 갈리지 않았다.
struct GraphicsPipelineDesc {
    const char* vertPath = nullptr;
    const char* fragPath = nullptr;

    // nullptr이면 정점 버퍼를 안 쓴다 (셰이더가 gl_VertexIndex로 만든다).
    const VkPipelineVertexInputStateCreateInfo* vertexInput = nullptr;

    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    // **UNDEFINED면 뎁스 첨부도 뎁스 테스트도 없다.** 둘을 따로 두면 "포맷은 줬는데
    // 테스트는 껐다" 같은 어긋난 조합이 생긴다.
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    // 셰이더가 정점 말고 무엇을 받나. 둘 다 없어도, 둘 다 있어도 된다.
    const VkPushConstantRange* pushConstants = nullptr;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
};

// 위 다섯을 뺀 나머지 - 두 파이프라인이 똑같이 쓰는 것들이 여기 한 번만 있다.
static bool CreateGraphicsPipeline(const VulkanDevice& dev,
                                   const GraphicsPipelineDesc& desc,
                                   Pipeline* out) noexcept {
    Pipeline& pipeline = *out;
    pipeline.dev = &dev;

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

    // 안 준 경우를 위한 빈 것. 정점 버퍼를 안 쓴다는 뜻이다.
    const VkPipelineVertexInputStateCreateInfo emptyVertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // 정점 3개 = 삼각형 1개

    // **뷰포트와 시저를 동적 상태로 둔다.** 여기 값을 박으면 창 크기가 바뀔 때마다
    // 파이프라인을 다시 만들어야 한다. 동적으로 두면 매 프레임 vkCmdSetViewport로 준다.
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
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;   // 뒷면도 그린다
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;               // **0이면 검증 레이어가 잡는다**

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;   // MSAA 없음

    // 블렌딩 없음. 그린 색으로 그대로 덮는다.
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                   | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    // **compareOp = LESS + 클리어 1.0**: 새 픽셀의 깊이가 기존보다 작을 때만 통과한다.
    // 그래서 가까운 것이 먼 것을 덮는다. depthWriteEnable을 끄고 정렬해 그리는 것이
    // 반투명을 다루는 방법이고, 그때 이 두 스위치가 갈린다.
    const bool useDepth = desc.depthFormat != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // 레이아웃: 셰이더가 받는 외부 자원(푸시 상수, 디스크립터)의 모양.
    // 둘 다 없으면 비어 있는 채로 만든다 - 그래도 만들어야 한다.
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

    // **다이나믹 렌더링**: VkRenderPass 객체를 안 만드는 대신 그릴 대상의 포맷을
    // 여기에 미리 알려준다. 파이프라인이 렌더 타겟 포맷에 묶이는 지점이 정확히 여기다.
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

    // **셰이더 모듈은 파이프라인이 만들어지고 나면 필요 없다.** 코드가 파이프라인 안으로
    // 컴파일돼 들어갔다.
    dev.table.vkDestroyShaderModule(dev.handle, vs, nullptr);
    dev.table.vkDestroyShaderModule(dev.handle, fs, nullptr);

    if (created != VK_SUCCESS) {
        LOG("[vk] vkCreateGraphicsPipelines failed (%d): %s\n", created, desc.vertPath);
        return false;   // layout은 ~Pipeline이 정리한다
    }
    return true;
}

// ---- 장면용: 정점 버퍼를 읽고 뎁스 테스트를 한다 ----
bool CreateTrianglePipeline(const VulkanDevice& dev,
                            RenderTargetFormats formats,
                            Pipeline* out) noexcept {
    // ---- 정점 입력: GPU에게 "정점 데이터를 어떻게 읽어라"를 알려준다 ----
    //
    // binding  버퍼 슬롯 하나. stride는 한 정점의 크기.
    // attribute 그 안의 필드 하나. location은 셰이더의 layout(location=N) in과 짝이다.
    //
    // **format이 크기까지 정한다**: 셰이더가 vec3으로 받아도 여기가 vec2면 z는 0이 된다 -
    // 조용히 틀리는 자리라 offsetof로 묶어둔다.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[2]{};
    attributes[0].location = 0;                             // layout(location = 0) in vec3
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);
    attributes[1].location = 1;                             // layout(location = 1) in vec3
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(std::size(attributes));
    vertexInput.pVertexAttributeDescriptions = attributes;

    // **stageFlags가 실제로 읽는 스테이지와 맞아야 한다.** 정점 셰이더만 쓰는데
    // FRAGMENT까지 켜면 낭비고, 빠뜨리면 검증 레이어가 잡는다.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    GraphicsPipelineDesc desc;
    desc.vertPath = "Shaders/triangle.vert.spv";
    desc.fragPath = "Shaders/triangle.frag.spv";
    desc.vertexInput = &vertexInput;
    desc.colorFormat = formats.color;
    desc.depthFormat = formats.depth;
    desc.pushConstants = &pushRange;

    if (!CreateGraphicsPipeline(dev, desc, out)) { return false; }
    LOG("[vk] triangle pipeline ready\n");
    return true;
}

// ---- 전체화면용: 정점도 뎁스도 없고, 대신 이미지를 읽는다 ----
bool CreateFullscreenPipeline(const VulkanDevice& dev,
                              VkFormat colorFormat,
                              VkDescriptorSetLayout setLayout,
                              Pipeline* out) noexcept {
    GraphicsPipelineDesc desc;
    desc.vertPath = "Shaders/fullscreen.vert.spv";
    desc.fragPath = "Shaders/fullscreen.frag.spv";
    // vertexInput 없음   - 셰이더가 gl_VertexIndex로 세 점을 만든다
    // depthFormat 없음   - 화면을 덮는 삼각형에 깊이 비교는 의미가 없다
    desc.colorFormat = colorFormat;   // **스왑체인 포맷이다.** 계약의 상대가 다르다
    desc.setLayout = setLayout;

    if (!CreateGraphicsPipeline(dev, desc, out)) { return false; }
    LOG("[vk] fullscreen pipeline ready\n");
    return true;
}

Pipeline::~Pipeline() {
    if (dev == nullptr) { return; }
    if (handle != VK_NULL_HANDLE) {
        dev->table.vkDestroyPipeline(dev->handle, handle, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        dev->table.vkDestroyPipelineLayout(dev->handle, layout, nullptr);
    }
}
