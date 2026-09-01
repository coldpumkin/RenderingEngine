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
        if (b->set != 0) { continue; }
        if (b->binding >= kMaxBindingsPerSet) {
            LOG("[vk] %s uses binding %u, over the %u we allow\n",
                path, b->binding, kMaxBindingsPerSet);
            spvReflectDestroyShaderModule(&module);
            return false;
        }
        // Indexed by binding number, not by order, so a gap stays a gap.
        out->bindingTypes[b->binding] = static_cast<VkDescriptorType>(b->descriptor_type);
        if (b->binding + 1 > out->bindingCount) { out->bindingCount = b->binding + 1; }
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
