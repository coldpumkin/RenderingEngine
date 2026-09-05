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

    Interpolation how = Interpolation::Smooth;
    if ((v.decoration_flags & SPV_REFLECT_DECORATION_FLAT) != 0) {
        how = Interpolation::Flat;
    } else if ((v.decoration_flags & SPV_REFLECT_DECORATION_NOPERSPECTIVE) != 0) {
        how = Interpolation::NoPerspective;
    }
    return {v.location, kind, components, how};
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

    // Which stage this is, read unconditionally. It used to be picked up only inside
    // the push-block branch, which left a stage that declares no push constants
    // reporting 0 -- and then the argument position was the only thing that said what
    // a .spv was. Feeding all 8x8 pairs of our shaders to CheckStageInterface let 8
    // through, 4 of them nonsense (two vertex, two fragment, one reversed): every one
    // a stage error, none an interface error.
    out->stage = static_cast<VkShaderStageFlags>(module.shader_stage);

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

    // Gaps, both ends, before anyone else sees this. A gap is a fact about this one
    // .spv -- no resource, no partner stage -- so it is answerable here, and here is
    // once per file rather than once per pipeline built from it.
    //
    // Vulkan is fine with a shader declaring locations 0 and 2. We are not: in these
    // shaders a gap means a location was removed and the other side not followed.
    // Ours to relax if a real one ever turns up.
    if (out->inputCount != out->maxInputLocation) {
        LOG("[vk] %s has gaps in its input locations (%u inputs, highest is %u)\n",
            path, out->inputCount, out->maxInputLocation);
        spvReflectDestroyShaderModule(&module);
        return false;
    }
    if (out->outputCount != out->maxOutputLocation) {
        LOG("[vk] %s has gaps in its output locations (%u outputs, highest is %u)\n",
            path, out->outputCount, out->maxOutputLocation);
        spvReflectDestroyShaderModule(&module);
        return false;
    }

    spvReflectDestroyShaderModule(&module);
    return true;
}

}   // namespace

const char* StageName(VkShaderStageFlags stage) noexcept {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:                  return "vertex";
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return "tess control";
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tess evaluation";
        case VK_SHADER_STAGE_GEOMETRY_BIT:                return "geometry";
        case VK_SHADER_STAGE_FRAGMENT_BIT:                return "fragment";
        case VK_SHADER_STAGE_COMPUTE_BIT:                 return "compute";
        default:                                          return "unknown";
    }
}

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
                    const ProgramStage stages[], uint32_t stageCount,
                    uint32_t set, DescriptorLayout* out) noexcept {
    // The union's end. Each stage reports how far its own declarations reach, and a
    // set reaches as far as the furthest of them.
    uint32_t count = 0;
    for (uint32_t s = 0; s < stageCount; ++s) {
        const uint32_t reach = stages[s].interface.sets[set].bindingCount;
        if (reach > count) { count = reach; }
    }

    VkDescriptorSetLayoutBinding bindings[kMaxBindingsPerSet]{};
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        // 0 reads as "this stage does not use it". A lone SAMPLER is also 0, and we
        // never declare one, so the two need not be told apart.
        VkDescriptorType type = static_cast<VkDescriptorType>(0);
        VkShaderStageFlags stageFlags = 0;
        for (uint32_t s = 0; s < stageCount; ++s) {
            const VkDescriptorType declared = stages[s].interface.sets[set].bindingTypes[i];
            if (declared == 0) { continue; }
            type = declared;   // the last stage that declares it wins, as before
            stageFlags |= stages[s].interface.stage;
        }
        if (stageFlags == 0) { continue; }   // a hole in the numbering

        bindings[used].binding = i;
        bindings[used].descriptorType = type;
        bindings[used].descriptorCount = 1;
        bindings[used].stageFlags = stageFlags;
        out->types[i] = type;
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
// Neighbours in the chain, not vertex-and-fragment: three things every pair agrees on,
// and they are that the producer's outputs are the consumer's inputs. Which two stages
// those are is the chain's business.
//
// On top of those three sits whatever the pair crosses, and the pairs do not all cross
// the same thing:
//
//   VS -> GS/TES    primitive assembly    the consumer's side becomes an array
//   ... -> FS       the rasterizer        interpolation, and both sides must say the same
//
// crossesRasterizer is the second one. The first has no case here yet -- every chain we
// build is one or two stages long -- and the array traits reflection reports are
// therefore not read. Measured: a geometry stage reads vColor as OpTypeArray %v3float
// %uint_3 where the vertex stage wrote a plain %v3float, at the same location, kind and
// component count. So the day a third stage exists, this function needs that arm too.
static bool CheckStageInterface(const ProgramStage& producer,
                                const ProgramStage& consumer,
                                bool crossesRasterizer) noexcept {
    const ShaderInterface& vs = producer.interface;
    const ShaderInterface& fs = consumer.interface;
    const char* vertPath = producer.path;
    const char* fragPath = consumer.path;

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

        // Only across the rasterizer, because only there is anything interpolated.
        // Between two pre-raster stages the value is handed over per vertex and the
        // decoration means nothing, so comparing it there would refuse a legal chain.
        //
        // glslc already refuses an integer fragment input that is not flat -- measured,
        // "'int' : must be qualified as flat in". What it cannot see is this: it
        // compiles one file at a time, so a producer that says flat and a consumer that
        // does not both compile clean and disagree only once they are put together.
        if (crossesRasterizer && written->interpolation != read.interpolation) {
            LOG("[vk] location %u: %s writes it %s, %s reads it %s\n",
                read.location, vertPath, InterpolationName(written->interpolation),
                fragPath, InterpolationName(read.interpolation));
            return false;
        }
    }
    return true;
}

bool CreateShaderProgram(const VulkanDevice& dev,
                         const char* const paths[], uint32_t count,
                         ShaderProgram* out) noexcept {
    out->dev = &dev;   // set first: the destructor runs even if this fails halfway

    if (count == 0 || count > kMaxStagesPerProgram) {
        LOG("[vk] a program of %u stages, and we hold %u\n", count, kMaxStagesPerProgram);
        return false;
    }

    // Load first, then sort. What order the caller listed them in is not information:
    // each .spv says which stage it is, and that is what the chain is built from.
    for (uint32_t i = 0; i < count; ++i) {
        out->stages[i].path = paths[i];
        out->stages[i].module = LoadShader(dev, paths[i], &out->stages[i].interface);
        if (out->stages[i].module == VK_NULL_HANDLE) {
            out->stageCount = i + 1;   // so the destructor frees what did load
            return false;
        }
    }
    out->stageCount = count;

    // Insertion sort by stage bit, which is pipeline order. Five elements at most.
    for (uint32_t i = 1; i < count; ++i) {
        ProgramStage held = out->stages[i];
        uint32_t j = i;
        while (j > 0 && out->stages[j - 1].interface.stage > held.interface.stage) {
            out->stages[j] = out->stages[j - 1];
            --j;
        }
        out->stages[j] = held;
    }

    // One chain, and the sort makes that a walk. Equal neighbours are the same stage
    // twice; compute sorts above every graphics bit, so it is a chain of one or an
    // error either way.
    for (uint32_t i = 0; i < count; ++i) {
        const VkShaderStageFlags stage = out->stages[i].interface.stage;
        if (stage == 0) {
            LOG("[vk] %s reports no stage\n", out->stages[i].path);
            return false;
        }
        if (i > 0 && stage == out->stages[i - 1].interface.stage) {
            LOG("[vk] %s and %s are both %s stages\n",
                out->stages[i - 1].path, out->stages[i].path, StageName(stage));
            return false;
        }
        if (count > 1 && stage == VK_SHADER_STAGE_COMPUTE_BIT) {
            LOG("[vk] %s is compute, which takes no other stage beside it\n",
                out->stages[i].path);
            return false;
        }
    }

    // Neighbours agree, and what they agree on depends on what the pair crosses.
    // Applied to a chain of one it says nothing, which is right: a lone stage has no
    // partner to disagree with.
    //
    // The pair whose consumer is the fragment stage is the one crossing the rasterizer,
    // and a chain has at most one -- there is one fragment stage or none. That makes
    // this the seam where interpolation is a question, and shadow's single stage the
    // case where it is not.
    for (uint32_t i = 1; i < count; ++i) {
        const bool crossesRasterizer =
            out->stages[i].interface.stage == VK_SHADER_STAGE_FRAGMENT_BIT;
        if (!CheckStageInterface(out->stages[i - 1], out->stages[i], crossesRasterizer)) {
            return false;
        }
    }

    for (uint32_t set = 0; set < kMaxSets; ++set) {
        if (!BuildSetLayout(dev, out->stages, out->stageCount, set,
                            &out->setLayouts[set])) {
            return false;
        }
    }

    // One range, covering what every stage together reaches. Each stage reports only
    // itself: the flags are or-ed and the sizes maxed, over however many there are.
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
    // The stages do not declare the same block and do not have to. What they owe each
    // other is that whatever each names sits at the offset the CPU wrote it to, which
    // is a contract in the shaders and unchecked here -- the .spv reports an extent,
    // and a field inside it is past what either side can see.
    VkPushConstantRange pushRange{};
    for (uint32_t i = 0; i < out->stageCount; ++i) {
        const ShaderInterface& stage = out->stages[i].interface;
        pushRange.stageFlags |= stage.PushStages();
        if (stage.pushSize > pushRange.size) { pushRange.size = stage.pushSize; }
    }

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
        LOG("[vk] vkCreatePipelineLayout failed: %s\n", out->stages[0].path);
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
    for (uint32_t i = 0; i < stageCount; ++i) {
        vk.vkDestroyShaderModule(dev->handle, stages[i].module, nullptr);
    }
}

// Every format anything here names, and nothing else. Unknown is a refusal rather than
// a guess: a format nobody classified would otherwise pass a check by accident.
//
// One table for both answers, because a format states them together and a shader type
// states them together. Two functions would be two switches over these same cases, and
// the second one is the half that used to be a literal at the call site.
//
// Grouped by the pair rather than by the kind: R32G32B32_SFLOAT and R32G32B32A32_SFLOAT
// are the same kind and not the same format to a shader.
FormatChannels ChannelsOfFormat(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R32_SFLOAT:
            return {NumericKind::Float, 1};
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32G32_SFLOAT:
            return {NumericKind::Float, 2};
        case VK_FORMAT_R32G32B32_SFLOAT:
            return {NumericKind::Float, 3};
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return {NumericKind::Float, 4};

        case VK_FORMAT_R32_UINT:
            return {NumericKind::Uint, 1};
        case VK_FORMAT_R32G32_UINT:
            return {NumericKind::Uint, 2};
        case VK_FORMAT_R32G32B32_UINT:
            return {NumericKind::Uint, 3};
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32G32B32A32_UINT:
            return {NumericKind::Uint, 4};

        case VK_FORMAT_R32_SINT:
            return {NumericKind::Sint, 1};
        case VK_FORMAT_R32G32_SINT:
            return {NumericKind::Sint, 2};
        case VK_FORMAT_R32G32B32_SINT:
            return {NumericKind::Sint, 3};
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32G32B32A32_SINT:
            return {NumericKind::Sint, 4};

        default:
            return {};
    }
}
