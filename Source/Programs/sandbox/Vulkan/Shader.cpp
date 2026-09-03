#include "Vulkan/Shader.h"

#include <spirv_reflect.h>

#include <cstdio>
#include <vector>

namespace {

// SPIR-V is an array of 32-bit words, so a size that is not a multiple of 4 means a
// broken file. The vector is uint32_t because vkCreateShaderModule's pCode requires
// 4-byte alignment and a char array plus a cast does not guarantee it.
bool ReadSpirv(const char* path, std::vector<uint32_t>* out) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        LOG("[vk] cannot open shader: %s\n", path);
        return false;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (size <= 0 || (size % 4) != 0) {
        LOG("[vk] bad SPIR-V size %ld: %s\n", size, path);
        std::fclose(file);
        return false;
    }

    out->resize(static_cast<size_t>(size) / 4);
    const size_t read = std::fread(out->data(), 1, static_cast<size_t>(size), file);
    std::fclose(file);
    if (read != static_cast<size_t>(size)) {
        LOG("[vk] short read: %s\n", path);
        return false;
    }
    return true;
}

bool Reflect(const std::vector<uint32_t>& code, const char* path,
             ShaderInterface* out) noexcept {
    SpvReflectShaderModule module{};
    if (spvReflectCreateShaderModule(code.size() * sizeof(uint32_t), code.data(), &module)
            != SPV_REFLECT_RESULT_SUCCESS) {
        LOG("[vk] spvReflectCreateShaderModule failed: %s\n", path);
        return false;
    }

    uint32_t inputCount = 0;
    spvReflectEnumerateInputVariables(&module, &inputCount, nullptr);
    std::vector<SpvReflectInterfaceVariable*> inputs(inputCount);
    if (inputCount != 0) {
        spvReflectEnumerateInputVariables(&module, &inputCount, inputs.data());
    }
    for (const SpvReflectInterfaceVariable* v : inputs) {
        // gl_VertexIndex and friends carry no location and are not vertex attributes.
        if (v->built_in != -1) { continue; }
        out->inputCount += 1;
        if (v->location + 1 > out->maxInputLocation) { out->maxInputLocation = v->location + 1; }
    }

    // Same enumeration on the other end of the boundary. A vertex stage reports none,
    // which is right: only the fragment stage writes attachments.
    uint32_t outputCount = 0;
    spvReflectEnumerateOutputVariables(&module, &outputCount, nullptr);
    std::vector<SpvReflectInterfaceVariable*> outputs(outputCount);
    if (outputCount != 0) {
        spvReflectEnumerateOutputVariables(&module, &outputCount, outputs.data());
    }
    for (const SpvReflectInterfaceVariable* v : outputs) {
        // gl_FragDepth and friends carry no location and are not attachments.
        if (v->built_in != -1) { continue; }
        out->outputCount += 1;
        if (v->location + 1 > out->maxOutputLocation) { out->maxOutputLocation = v->location + 1; }
    }

    uint32_t blockCount = 0;
    spvReflectEnumeratePushConstantBlocks(&module, &blockCount, nullptr);
    std::vector<SpvReflectBlockVariable*> blocks(blockCount);
    if (blockCount != 0) {
        spvReflectEnumeratePushConstantBlocks(&module, &blockCount, blocks.data());
        out->pushSize = blocks[0]->size;
        // The stage is the shader's own. Two stages sharing one block each report
        // themselves, and the pipeline ors them together.
        out->pushStages = static_cast<VkShaderStageFlags>(module.shader_stage);
    }

    uint32_t bindingCount = 0;
    spvReflectEnumerateDescriptorBindings(&module, &bindingCount, nullptr);
    std::vector<SpvReflectDescriptorBinding*> bindings(bindingCount);
    if (bindingCount != 0) {
        spvReflectEnumerateDescriptorBindings(&module, &bindingCount, bindings.data());
    }
    for (const SpvReflectDescriptorBinding* b : bindings) {
        if (b->set >= kMaxSets) {
            LOG("[vk] %s uses set %u, over the %u we allow\n", path, b->set, kMaxSets);
            spvReflectDestroyShaderModule(&module);
            return false;
        }
        if (b->binding >= kMaxBindingsPerSet) {
            LOG("[vk] %s uses binding %u, over the %u we allow\n",
                path, b->binding, kMaxBindingsPerSet);
            spvReflectDestroyShaderModule(&module);
            return false;
        }
        // Indexed by binding number, not by order, so a gap stays a gap.
        SetInterface& set = out->sets[b->set];
        set.bindingTypes[b->binding] = static_cast<VkDescriptorType>(b->descriptor_type);
        if (b->binding + 1 > set.bindingCount) { set.bindingCount = b->binding + 1; }
    }

    spvReflectDestroyShaderModule(&module);
    return true;
}

}   // namespace

bool ReflectShaderFile(const char* path, ShaderInterface* out) noexcept {
    std::vector<uint32_t> code;
    if (!ReadSpirv(path, &code)) { return false; }
    return Reflect(code, path, out);
}

VkShaderModule LoadShader(const VulkanDevice& dev, const char* path,
                          ShaderInterface* out) noexcept {
    std::vector<uint32_t> code;
    if (!ReadSpirv(path, &code)) { return VK_NULL_HANDLE; }
    if (!Reflect(code, path, out)) { return VK_NULL_HANDLE; }

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(uint32_t);   // bytes, not words
    info.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (dev.table.vkCreateShaderModule(dev.handle, &info, nullptr, &module) != VK_SUCCESS) {
        LOG("[vk] vkCreateShaderModule failed: %s\n", path);
        return VK_NULL_HANDLE;
    }
    return module;
}

bool BuildSetLayout(const VulkanDevice& dev,
                    const ShaderInterface& vert, const ShaderInterface& frag,
                    uint32_t set, DescriptorLayout* out) noexcept {
    const SetInterface& vertSet = vert.sets[set];
    const SetInterface& fragSet = frag.sets[set];

    const uint32_t count = vertSet.bindingCount > fragSet.bindingCount
                         ? vertSet.bindingCount : fragSet.bindingCount;
    VkDescriptorSetLayoutBinding bindings[kMaxBindingsPerSet]{};
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        // 0 reads as "this stage does not use it". A lone SAMPLER is also 0, and we
        // never declare one, so the two need not be told apart.
        const VkDescriptorType inVert = vertSet.bindingTypes[i];
        const VkDescriptorType inFrag = fragSet.bindingTypes[i];
        if (inVert == 0 && inFrag == 0) { continue; }   // a hole in the numbering

        bindings[used].binding = i;
        bindings[used].descriptorType = inFrag != 0 ? inFrag : inVert;
        bindings[used].descriptorCount = 1;
        bindings[used].stageFlags = (inVert != 0 ? VK_SHADER_STAGE_VERTEX_BIT : 0)
                                  | (inFrag != 0 ? VK_SHADER_STAGE_FRAGMENT_BIT : 0);
        out->types[i] = bindings[used].descriptorType;
        ++used;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = used;
    layoutInfo.pBindings = bindings;
    if (dev.table.vkCreateDescriptorSetLayout(dev.handle, &layoutInfo, nullptr, &out->handle)
            != VK_SUCCESS) {
        LOG("[vk] vkCreateDescriptorSetLayout failed\n");
        return false;
    }
    out->bindingCount = count;
    return true;
}

bool CreateShaderProgram(const VulkanDevice& dev,
                         const char* vertPath, const char* fragPath,
                         ShaderProgram* out) noexcept {
    out->dev = &dev;   // set first: the destructor runs even if this fails halfway
    out->vertPath = vertPath;
    out->fragPath = fragPath;

    out->vert = LoadShader(dev, vertPath, &out->vertInterface);
    out->frag = LoadShader(dev, fragPath, &out->fragInterface);
    if (out->vert == VK_NULL_HANDLE || out->frag == VK_NULL_HANDLE) {
        return false;
    }

    for (uint32_t set = 0; set < kMaxSets; ++set) {
        if (!BuildSetLayout(dev, out->vertInterface, out->fragInterface, set,
                            &out->setLayouts[set])) {
            return false;
        }
    }

    // One block, however many stages read it. Each stage reports only itself, so the
    // flags are or-ed and the size is whichever declared one -- they are the same
    // block, and glslc rejects a disagreement inside the shaders.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = out->vertInterface.pushStages | out->fragInterface.pushStages;
    pushRange.size = out->vertInterface.pushSize > out->fragInterface.pushSize
                         ? out->vertInterface.pushSize
                         : out->fragInterface.pushSize;

    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (pushRange.size != 0) {
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &pushRange;
    }
    // Every set, including ones no shader declared: their layouts are empty, and the
    // position is what matters.
    VkDescriptorSetLayout setHandles[kMaxSets]{};
    for (uint32_t set = 0; set < kMaxSets; ++set) {
        setHandles[set] = out->setLayouts[set].handle;
    }
    info.setLayoutCount = kMaxSets;
    info.pSetLayouts = setHandles;

    // A layout is required even when every set is empty.
    if (dev.table.vkCreatePipelineLayout(dev.handle, &info, nullptr, &out->layout)
            != VK_SUCCESS) {
        LOG("[vk] vkCreatePipelineLayout failed: %s\n", vertPath);
        return false;
    }
    return true;
}

// The modules go with it, not after the first pipeline: a second variant built from
// this program needs them.
ShaderProgram::~ShaderProgram() {
    if (dev == nullptr) { return; }
    const VolkDeviceTable& vk = dev->table;
    vk.vkDestroyPipelineLayout(dev->handle, layout, nullptr);
    for (const DescriptorLayout& set : setLayouts) {
        vk.vkDestroyDescriptorSetLayout(dev->handle, set.handle, nullptr);
    }
    vk.vkDestroyShaderModule(dev->handle, vert, nullptr);
    vk.vkDestroyShaderModule(dev->handle, frag, nullptr);
}
