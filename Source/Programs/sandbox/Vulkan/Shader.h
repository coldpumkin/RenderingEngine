#pragma once

#include "Vulkan/Device.h"

#include <cstring>   // the binding names are copied, not pointed at

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

// A stage's outputs. For a fragment stage they are colour attachments; for a vertex
// stage they are varyings on their way to the next one. The same slots either way --
// what they mean is the stage's, not the slot's -- so one array and one ceiling.
// scene.vert declares four varyings and a G-buffer fragment stage would write three.
constexpr uint32_t kMaxOutputSlots = 8;

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

// How a value is carried across the rasterizer. It is a property of one seam and not
// of a slot in general -- a vertex input is read from a buffer and a fragment output
// is written to an image, and neither is interpolated. Read on every slot anyway,
// because reflection reports it per variable and the seam is what decides to compare.
//
// spirv-reflect reports these two. Centroid and Sample are SPIR-V decorations it does
// not surface, so a disagreement in those is past what this can see.
enum class Interpolation {
    Smooth,          // the default: perspective-correct
    NoPerspective,
    Flat,            // not interpolated at all -- one provoking vertex feeds every fragment
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
    Interpolation interpolation = Interpolation::Smooth;
};

// For messages. Here and not beside one of its callers, because both interface checks
// print it and they live in different files.
inline const char* KindName(NumericKind kind) noexcept {
    switch (kind) {
        case NumericKind::Float: return "float";
        case NumericKind::Sint:  return "sint";
        case NumericKind::Uint:  return "uint";
        default:                 return "unknown";
    }
}

// What a resource format and a shader type both state, and the only two things they
// both state.
//
// A VkFormat names three things -- which channels, how many bits each is in memory,
// and how those bits are read as a number. A shader type names two: how many
// components, and which kind of number. The bit width and the packing are missing from
// the second on purpose: the hardware converts, so one shader reads an 8-bit and a
// 32-bit resource with the same vec4, which is why the format belongs to the resource
// and the type to the shader.
//
// The conversion is free inside a kind and absent across one. UNORM, SNORM, SFLOAT and
// SRGB all deliver float; UINT delivers unsigned, SINT signed. So SRGB against UNORM is
// past what any check here can see -- same kind, same count -- and a normal map read as
// SRGB is silent for that reason.
struct FormatChannels {
    NumericKind kind = NumericKind::Unknown;
    uint32_t count = 0;              // 0 alongside Unknown: a format we did not classify
};

// Output: the pair above. Unknown/0 for anything not listed, which is a refusal rather
//         than a guess -- add the format there when one is used.
FormatChannels ChannelsOfFormat(VkFormat format) noexcept;

// What one set declares. Indexed by binding number, so a gap stays a gap.
//
// The name is kept because the type does not tell two bindings apart: four samplers in
// a row are one shape however they are ordered, and swapping two of them is a picture
// that is wrong and legal. The name is the only thing in a .spv that says which is
// which.
// Copied and not pointed at: reflection frees its module before this is read.
constexpr uint32_t kMaxBindingNameLength = 48;

// How many members of one uniform block we record. Our widest is ViewOptionsUniform
// at eight floats.
constexpr uint32_t kMaxBlockMembers = 8;

// One member of a uniform block, as the .spv declares it.
//
// **A stage declares only the members it reads**, so this is a subset of whatever the
// renderer's struct holds -- scene.frag takes viewPos out of CameraUniform and names
// neither matrix. offset is from the block's start, which is what makes it comparable
// to offsetof on the C++ side.
struct BlockMember {
    char name[kMaxBindingNameLength]{};
    uint32_t offset = 0;   // bytes from the start of the block
    uint32_t size = 0;     // bytes
};

struct SetInterface {
    uint32_t bindingCount = 0;
    VkDescriptorType bindingTypes[kMaxBindingsPerSet]{};
    char bindingNames[kMaxBindingsPerSet][kMaxBindingNameLength]{};

    // Filled for uniform buffer bindings only -- an image has no members. 0 means the
    // binding is not a block, or the compiler stripped the names.
    uint32_t memberCounts[kMaxBindingsPerSet]{};
    BlockMember members[kMaxBindingsPerSet][kMaxBlockMembers]{};
};

// What a caller requires of one set, when more than one program has to speak it.
//
// A set a single program uses is that program's own, and reflection is the whole of it.
// A set several programs must agree on is not: it is the renderer's, and each program
// makes a claim about it that this can refuse. Which sets are which is the caller's --
// this layer only compares.
// One member of a block, as the renderer declares it. **Written with offsetof and
// sizeof on the C++ struct**, never by hand: moving a field there moves this, and a
// shader that did not move with it is refused.
struct RequiredMember {
    const char* name = nullptr;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct RequiredSet {
    uint32_t set = 0;
    uint32_t bindingCount = 0;
    VkDescriptorType types[kMaxBindingsPerSet]{};
    const char* names[kMaxBindingsPerSet]{};   // nullptr in a slot skips the name check
};

// A uniform block the renderer owns, wherever a shader declares one.
//
// **Keyed by the name, not by a set and binding.** The same block sits at different
// slots in different programs -- ShadowUniform is binding 0 in shadow.vert and binding
// 2 in scene.frag -- so a requirement written against a slot could not reach both.
// What has to agree is the layout; where it is bound is each program's own business.
//
// That is the difference between this and RequiredSet beside it: a set requirement is
// about what a set is made of, and this is about what one block looks like inside.
// A program can be held to both, one, or neither.
//
// **A stage is held to a subset.** Every member it declares has to appear here at the
// same offset and size, and one it declares that is not here is refused -- that being
// the direction the whole boundary runs. A member no stage reads is compared by
// nobody, which is why padding can sit in the struct and in no shader.
struct RequiredBlock {
    const char* name = nullptr;   // the instance name a shader gives it, e.g. "camera"
    const RequiredMember* members = nullptr;
    uint32_t memberCount = 0;
};

// What a caller holds a program to. Both halves are optional -- a program sharing
// nothing passes an empty one -- and they answer different questions, so a program
// may be held to blocks without being held to any set.
struct ProgramRequirements {
    const RequiredSet* sets = nullptr;
    uint32_t setCount = 0;
    const RequiredBlock* blocks = nullptr;
    uint32_t blockCount = 0;
};

struct ShaderInterface {
    // Which stage this .spv is, out of the .spv. Exactly one bit -- a module has one
    // stage -- and the bits are already in pipeline order:
    //
    //   VERTEX 0x1 < TESC 0x2 < TESE 0x4 < GEOMETRY 0x8 < FRAGMENT 0x10
    //   COMPUTE 0x20 alone, no neighbours
    //
    // 0 means reflection did not report one, which is a refusal: without it the
    // argument position is the only thing saying what a file is.
    VkShaderStageFlags stage = 0;

    // Reflection refuses a gap, so these two are equal by the time anyone reads them.
    // The second is kept because that is what the refusal is measured against.
    uint32_t inputCount = 0;         // built-ins excluded
    uint32_t maxInputLocation = 0;   // highest location + 1

    // In declaration order, not indexed by location -- the caller looks a location up
    // rather than assuming the two coincide.
    InterfaceSlot inputs[kMaxVertexAttributes]{};

    // What this stage writes, whatever that turns out to be for its stage: colour
    // attachments from a fragment stage, varyings from a vertex one. Read for both
    // now -- a vertex stage's outputs used to be skipped, which left the boundary
    // between the two stages the only one nothing could see.
    uint32_t outputCount = 0;
    uint32_t maxOutputLocation = 0;
    InterfaceSlot outputs[kMaxOutputSlots]{};

    uint32_t pushSize = 0;                    // 0 when the stage declares no block

    SetInterface sets[kMaxSets];

    // Derived, not stored: a stage contributes itself to the push range exactly when
    // it declares a block. Two fields would let "declares one but contributes none"
    // be expressible.
    VkShaderStageFlags PushStages() const noexcept { return pushSize != 0 ? stage : 0; }
};

// For messages. A stage is one bit, so this is a lookup, not a decomposition.
const char* StageName(VkShaderStageFlags stage) noexcept;

// For messages, and the words are GLSL's.
inline const char* InterpolationName(Interpolation how) noexcept {
    switch (how) {
        case Interpolation::NoPerspective: return "noperspective";
        case Interpolation::Flat:          return "flat";
        default:                           return "smooth";
    }
}

// A set layout and what the shaders asked for. The handle is opaque, so none of
// this can be asked back for. types is indexed by binding number; a gap is left at 0.
struct DescriptorLayout {
    VkDescriptorSetLayout handle = VK_NULL_HANDLE;
    uint32_t bindingCount = 0;
    VkDescriptorType types[kMaxBindingsPerSet]{};
};

// Effect: builds one set's layout from what every stage declares between them. All are
//         read: vertex asking for a uniform and fragment for a sampler is the usual
//         shape, and the layout is their union.
//
// A set no stage declares still gets a layout, with no bindings in it. Vulkan numbers
// sets by position, so set 1 cannot be handed over without a set 0 beside it.
struct ProgramStage;
bool BuildSetLayout(const VulkanDevice& dev,
                    const ProgramStage stages[], uint32_t stageCount,
                    uint32_t set, DescriptorLayout* out) noexcept;

// Effect: reads path and fills out. No device involved.
// Output: false on a missing or malformed .spv, more than kMaxBindingsPerSet, or a gap
//         in the input or output locations.
//
// Every refusal here is a fact about this one file -- no resource, no partner stage --
// which is why it is answerable without either. Checks that need a second side live
// where that side arrives: a partner stage at CreateShaderProgram, a VertexLayout or
// an AttachmentFormats at CreateGraphicsPipeline.
bool ReflectShaderFile(const char* path, ShaderInterface* out) noexcept;

// Same file, plus the VkShaderModule. Reads the file once for both.
//
// Output: VK_NULL_HANDLE on failure
VkShaderModule LoadShader(const VulkanDevice& dev, const char* path,
                          ShaderInterface* out) noexcept;


// ShaderProgram - a chain of stages, and everything Vulkan wants before a pipeline
// ============================================================================
//
// The interface, split from the variant. Everything in here comes out of the .spv and
// nothing comes from GraphicsPipelineDesc -- that is the whole line.
//
// It is a type because it belongs to a pass, not to a pipeline. Pipelines built from
// one set of shaders differ only in baked state (polygon mode, blend),
// and they have to share this: a set drawn from one of these layouts gets bound
// through this pipeline layout, whichever pipeline is current. Vulkan calls two
// layouts compatible when identically defined -- nothing checks the "identically", and
// one object removes the question.
//
// The modules stay alive with it. A second variant should not reread the file, and
// they cost nothing to keep.

// How many stages one program may hold. The graphics chain is five long; compute is
// one and stands alone.
constexpr uint32_t kMaxStagesPerProgram = 5;

// One .spv, everything we keep about it. The three used to be three fields each,
// named for a stage -- which is what made "a program is a pair" true by construction.
struct ProgramStage {
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderInterface interface;

    // For logs. A string literal from the call site, so holding the pointer is free.
    const char* path = nullptr;
};

struct ShaderProgram {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    // In stage-bit order, which is pipeline order: the bits are already sorted that
    // way (VERTEX 0x1 ... FRAGMENT 0x10). So "the stage after this one" is the next
    // element, and the two ends of the chain are the two whose partner is the CPU --
    // stages[0] answers to a VertexLayout, the last one to an AttachmentFormats.
    ProgramStage stages[kMaxStagesPerProgram];
    uint32_t stageCount = 0;

    // Output: the stage with this bit, or nullptr. Callers that need a particular one
    //         ask for it rather than indexing: a fragment stage is not always last
    //         (depth-only has none) and never at a fixed position.
    const ProgramStage* Stage(VkShaderStageFlags stage) const noexcept {
        for (uint32_t i = 0; i < stageCount; ++i) {
            if (stages[i].interface.stage == stage) { return &stages[i]; }
        }
        return nullptr;
    }

    // One per set the shaders may declare, in set order. A set nothing declares still
    // gets an entry with no bindings: Vulkan numbers sets by position, so set 1
    // cannot be handed to vkCreatePipelineLayout without a set 0 in front of it.
    //
    // Which set means what is not decided here. The shaders declare positions; the
    // layer that wrote those shaders is where the positions get names.
    DescriptorLayout setLayouts[kMaxSets];
    VkPipelineLayout layout = VK_NULL_HANDLE;

    ShaderProgram() = default;
    ~ShaderProgram();
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
};

// Effect: loads every stage, reads what they declare, and builds the set layouts and
//         the pipeline layout from it. Order of the paths does not matter -- each
//         .spv says which stage it is and they are sorted into chain order.
//
// Output: false unless the stages form one chain: no stage twice, and compute alone if
//         present. Gaps are not an error -- vertex straight to fragment is the chain
//         four of ours are.
//
// required names the sets this program shares with others, and each one is compared
// against what the shaders declared rather than built from it. A program that shares
// nothing passes none.
//
// Contract: the paths must outlive this -- they are kept for logging.
bool CreateShaderProgram(const VulkanDevice& dev,
                         const char* const paths[], uint32_t count,
                         const ProgramRequirements& required,
                         ShaderProgram* out) noexcept;
