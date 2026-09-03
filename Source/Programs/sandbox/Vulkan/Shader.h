#pragma once

#include "Vulkan/Device.h"

// Shader - what a .spv declares, read out of the SPIR-V itself
// ============================================================================
//
//   location . format . push size . push stages . binding set/type   the .spv knows
//   attribute offset . vertex stride                                 only Vertex knows
//
// The second row is packing, and no shader declares it: one shader can read buffers
// laid out differently. So vertex layouts still need Vertex; push ranges and set
// layouts need nothing else.
constexpr uint32_t kMaxBindingsPerSet = 8;   // ceiling we impose, not a counted value

// Sets are read, not just set 0, because how many sets exist is not a preference:
// a binding's count comes from somewhere, and bindings whose counts come from
// different places cannot share a set without multiplying. What those places are is
// the caller's business -- this layer only reports which sets a shader declared.
constexpr uint32_t kMaxSets = 2;

// Ceilings we impose. Our two vertex types declare four and three attributes.
constexpr uint32_t kMaxVertexAttributes = 8;

// Fragment outputs. One today, and the number a G-buffer would need is the reason
// this is not 1.
constexpr uint32_t kMaxColorOutputs = 4;

// What kind of number a shader variable is made of, and what a resource format
// converts to. Vulkan converts freely inside a kind -- R8G8B8A8_UNORM feeds a vec4 --
// and not at all across one: an integer attribute cannot feed a float input.
//
// So this, not the format, is what the two sides have to agree on. A shader is never
// UNORM; that word belongs to the resource.
enum class NumericKind {
    Unknown,
    Float,
    Sint,
    Uint,
};

// One end of one location, as the .spv declares it. Not a VkFormat -- a shader has no
// format, it has a kind and a width in components.
//
// The same type at both ends, because both ends are the same question: a vertex input
// asks what the buffer delivers at a location, a fragment output says what it writes
// at one. What differs is on the resource side, not here -- a buffer adds an offset
// and a stride, an image adds a sample count.
struct InterfaceSlot {
    uint32_t location = 0;
    NumericKind kind = NumericKind::Unknown;
    uint32_t componentCount = 0;
};

// Output: what a resource format delivers to a shader. Unknown for anything not
//         listed, which is a refusal rather than a guess -- add the format here when
//         one is used.
NumericKind KindOfFormat(VkFormat format) noexcept;

// What one set declares. types is indexed by binding number, so a gap stays a gap.
struct SetInterface {
    uint32_t bindingCount = 0;
    VkDescriptorType bindingTypes[kMaxBindingsPerSet]{};
};

struct ShaderInterface {
    uint32_t inputCount = 0;         // vertex attributes, built-ins excluded
    uint32_t maxInputLocation = 0;   // highest location + 1, so gaps show up

    // In declaration order, not indexed by location -- the caller looks a location up
    // rather than assuming the two coincide.
    InterfaceSlot inputs[kMaxVertexAttributes]{};

    // The other end of the same boundary, read the same way and kept the same way.
    // The counts alone were enough while "how many colour attachments" was the only
    // question anyone asked of a fragment stage; what each output is made of is the
    // half that lets the check here be the mirror of the one on the inputs.
    uint32_t outputCount = 0;
    uint32_t maxOutputLocation = 0;
    InterfaceSlot outputs[kMaxColorOutputs]{};

    uint32_t pushSize = 0;                    // 0 when the stage declares no block
    VkShaderStageFlags pushStages = 0;        // the stage itself, if it reads one

    SetInterface sets[kMaxSets];
};

// A set layout and what the shaders asked for. The handle is opaque, so none of
// this can be asked back for. types is indexed by binding number; a gap is left at 0.
struct DescriptorLayout {
    VkDescriptorSetLayout handle = VK_NULL_HANDLE;
    uint32_t bindingCount = 0;
    VkDescriptorType types[kMaxBindingsPerSet]{};
};

// Effect: builds one set's layout from what the two stages declare between them. Both
//         are read: vertex asking for a uniform and fragment for a sampler is the
//         usual shape.
//
// A set neither stage declares still gets a layout, with no bindings in it. Vulkan
// numbers sets by position, so set 1 cannot be handed over without a set 0 beside it.
bool BuildSetLayout(const VulkanDevice& dev,
                    const ShaderInterface& vert, const ShaderInterface& frag,
                    uint32_t set, DescriptorLayout* out) noexcept;

// Effect: reads path and fills out. No device involved.
// Output: false on a missing or malformed .spv, or more than kMaxBindingsPerSet.
bool ReflectShaderFile(const char* path, ShaderInterface* out) noexcept;

// Same file, plus the VkShaderModule. Reads the file once for both.
//
// Output: VK_NULL_HANDLE on failure
VkShaderModule LoadShader(const VulkanDevice& dev, const char* path,
                          ShaderInterface* out) noexcept;


// ShaderProgram - a pair of shaders, and everything Vulkan wants before a pipeline
// ============================================================================
//
// The interface, split from the variant. Everything in here comes out of the .spv and
// nothing comes from GraphicsPipelineDesc -- that is the whole line.
//
// It is a type because it belongs to a pass, not to a pipeline. Pipelines built from
// one pair of shaders differ only in state the pass admits (polygon mode, blending),
// and they have to share this: a set drawn from one of these layouts gets bound
// through this pipeline layout, whichever pipeline is current. Vulkan calls two
// layouts compatible when identically defined -- nothing checks the "identically", and
// one object removes the question.
//
// The modules stay alive with it. A second variant should not reread the file, and
// they cost nothing to keep.
struct ShaderProgram {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    ShaderInterface vertInterface;
    ShaderInterface fragInterface;

    // One per set the shaders may declare, in set order. A set nothing declares still
    // gets an entry with no bindings: Vulkan numbers sets by position, so set 1
    // cannot be handed to vkCreatePipelineLayout without a set 0 in front of it.
    //
    // Which set means what is not decided here. The shaders declare positions; the
    // layer that wrote those shaders is where the positions get names.
    DescriptorLayout setLayouts[kMaxSets];
    VkPipelineLayout layout = VK_NULL_HANDLE;

    // For logs. String literals from the call site, so holding the pointers is free.
    const char* vertPath = nullptr;
    const char* fragPath = nullptr;

    ShaderProgram() = default;
    ~ShaderProgram();
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
};

// Effect: loads both stages, reads what they declare, and builds the set layouts and
//         the pipeline layout from it.
//
// Contract: the two paths must outlive this -- they are kept for logging.
bool CreateShaderProgram(const VulkanDevice& dev,
                         const char* vertPath, const char* fragPath,
                         ShaderProgram* out) noexcept;
