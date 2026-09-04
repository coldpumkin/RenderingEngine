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

// What one interface variable is, at either end. The kind is what the two sides have
// to agree on: Vulkan converts inside one and not across it.
//
// A scalar reports no vector width; it is one component.
InterfaceSlot SlotOf(const SpvReflectInterfaceVariable& v) noexcept {
    NumericKind kind = NumericKind::Unknown;
    if (v.type_description != nullptr) {
        const uint32_t flags = v.type_description->type_flags;
        if ((flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0) {
            kind = NumericKind::Float;
        } else if ((flags & SPV_REFLECT_TYPE_FLAG_INT) != 0) {
            kind = v.numeric.scalar.signedness != 0 ? NumericKind::Sint
                                                    : NumericKind::Uint;
        }
    }
    const uint32_t components = v.numeric.vector.component_count != 0
                              ? v.numeric.vector.component_count : 1;
    return {v.location, kind, components};
}

// Output: whether this variable sits at a location, which is what makes it part of an
//         interface we can check
//
// built_in alone is not enough. glslc gives a vertex stage a gl_PerVertex output block
// whose members are built-ins while **the block itself is not**, so it arrives with
// built_in -1 and no Location decoration -- spirv-reflect reports that as UINT32_MAX.
// Measured on fullscreen.vert: name empty, location 0xFFFFFFFF, built_in -1, and it
// was counted as a second varying until this line existed.
static bool HasLocation(const SpvReflectInterfaceVariable& v) noexcept {
    return v.built_in == -1 && v.location != UINT32_MAX;
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
        if (!HasLocation(*v)) { continue; }
        if (out->inputCount >= kMaxVertexAttributes) {
            LOG("[vk] %s declares more than %u vertex inputs\n", path, kMaxVertexAttributes);
            spvReflectDestroyShaderModule(&module);
            return false;
        }

        // The kind, not the format. A vec3 is three floats however the buffer stores
        // them, and that is the whole reason a layout can differ from the shader's own
        // idea of the type.
        out->inputs[out->inputCount] = SlotOf(*v);
        out->inputCount += 1;
        if (v->location + 1 > out->maxInputLocation) { out->maxInputLocation = v->location + 1; }
    }

    // The same enumeration on the other end, for every stage. What comes back is the
    // same SPIR-V storage class in both cases; where it lands is the stage's business
    // -- a fragment stage's outputs are attachments, a vertex stage's are varyings.
    uint32_t outputCount = 0;
    spvReflectEnumerateOutputVariables(&module, &outputCount, nullptr);
    std::vector<SpvReflectInterfaceVariable*> outputs(outputCount);
    if (outputCount != 0) {
        spvReflectEnumerateOutputVariables(&module, &outputCount, outputs.data());
    }
    for (const SpvReflectInterfaceVariable* v : outputs) {
        // gl_Position, gl_FragDepth and the block they arrive in go to the hardware.
        if (!HasLocation(*v)) { continue; }
        if (out->outputCount >= kMaxOutputSlots) {
            LOG("[vk] %s writes more than %u outputs\n", path, kMaxOutputSlots);
            spvReflectDestroyShaderModule(&module);
            return false;
        }
        out->outputs[out->outputCount] = SlotOf(*v);
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

// The one boundary with a shader at both ends
// ----------------------------------------------------------------------------
//
// The other interface checks live at pipeline creation because each has a CPU-side
// fact to compare against -- a VertexLayout, an AttachmentFormats. This one has none.
// What a vertex stage writes and what a fragment stage reads are both declared in
// SPIR-V and nothing on our side is party to it, so a check is the only thing that
// can see it at all.
//
// Here rather than in Pipeline.cpp for the same reason: it is a fact about the pair,
// not about a variant of the pair. The scene's two pipelines share one program and
// would otherwise ask this twice.
//
// Locations must match one for one. A varying written and never read is legal Vulkan
// and wasted interpolation; refusing it costs nothing here, since a stage that stops
// reading something is a stage whose partner should stop writing it.
static bool CheckStageInterface(const ShaderInterface& vs, const ShaderInterface& fs,
                                const char* vertPath, const char* fragPath) noexcept {
    if (vs.outputCount != fs.inputCount) {
        LOG("[vk] %s writes %u varyings and %s reads %u\n",
            vertPath, vs.outputCount, fragPath, fs.inputCount);
        return false;
    }

    for (uint32_t i = 0; i < fs.inputCount; ++i) {
        const InterfaceSlot& read = fs.inputs[i];

        const InterfaceSlot* written = nullptr;
        for (uint32_t j = 0; j < vs.outputCount; ++j) {
            if (vs.outputs[j].location == read.location) {
                written = &vs.outputs[j];
                break;
            }
        }
        if (written == nullptr) {
            LOG("[vk] %s reads location %u, which %s does not write\n",
                fragPath, read.location, vertPath);
            return false;
        }

        // The kind and the width, the same two the other two checks compare. A vec3
        // read as a vec4 leaves one component undefined and Vulkan does not say so.
        if (written->kind != read.kind) {
            LOG("[vk] location %u: %s writes %s, %s reads %s\n",
                read.location, vertPath, KindName(written->kind),
                fragPath, KindName(read.kind));
            return false;
        }
        if (written->componentCount != read.componentCount) {
            LOG("[vk] location %u: %s writes %u components, %s reads %u\n",
                read.location, vertPath, written->componentCount,
                fragPath, read.componentCount);
            return false;
        }
    }
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

    if (!CheckStageInterface(out->vertInterface, out->fragInterface,
                             vertPath, fragPath)) {
        return false;
    }

    for (uint32_t set = 0; set < kMaxSets; ++set) {
        if (!BuildSetLayout(dev, out->vertInterface, out->fragInterface, set,
                            &out->setLayouts[set])) {
            return false;
        }
    }

    // One range, covering what every stage together reaches. Each stage reports only
    // itself: the flags are or-ed, and the size is the larger of the two.
    //
    // The max is the union's end, not a guess between two numbers that ought to match.
    // A stage declares the fields it reads and reflection reports the block's extent,
    // which counts from 0 whether or not the stage names a field at offset 0 -- so a
    // fragment stage reading only "layout(offset = 112) float alpha" reports 116, and
    // a vertex stage reading the first three fields reports 112. Measured:
    //
    //   mesh      vert 112 B  frag 116 B  ->  116 B  VERTEX | FRAGMENT
    //   shadow    vert  64 B  frag   0 B  ->   64 B  VERTEX
    //
    // The two stages do not declare the same block and do not have to. What they owe
    // each other is that whatever each names sits at the offset the CPU wrote it to,
    // which is a contract in the shaders and unchecked here -- the .spv reports an
    // extent, and a field inside it is past what either side can see.
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

// Every format anything here puts in a VertexLayout, and nothing else. Unknown is a
// refusal rather than a guess: a format nobody classified would otherwise pass the
// check by accident.
//
// UNORM and SRGB are Float. What the bytes are is not what the shader reads -- the
// conversion belongs to the format, which is exactly why the shader's own type cannot
// stand in for this.
NumericKind KindOfFormat(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return NumericKind::Float;

        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT:
            return NumericKind::Uint;

        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT:
            return NumericKind::Sint;

        default:
            return NumericKind::Unknown;
    }
}
