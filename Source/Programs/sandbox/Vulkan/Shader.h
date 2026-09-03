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

// What one set declares. types is indexed by binding number, so a gap stays a gap.
struct SetInterface {
    uint32_t bindingCount = 0;
    VkDescriptorType bindingTypes[kMaxBindingsPerSet]{};
};

struct ShaderInterface {
    uint32_t inputCount = 0;         // vertex attributes, built-ins excluded
    uint32_t maxInputLocation = 0;   // highest location + 1, so gaps show up

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
