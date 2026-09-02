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

struct ShaderInterface {
    uint32_t inputCount = 0;         // vertex attributes, built-ins excluded
    uint32_t maxInputLocation = 0;   // highest location + 1, so gaps show up

    uint32_t pushSize = 0;                    // 0 when the stage declares no block
    VkShaderStageFlags pushStages = 0;        // the stage itself, if it reads one

    uint32_t bindingCount = 0;                // in set 0
    VkDescriptorType bindingTypes[kMaxBindingsPerSet]{};
};

// A set layout and what the shaders asked for. The handle is opaque, so none of
// this can be asked back for. types is indexed by binding number; a gap is left at 0.
struct DescriptorLayout {
    VkDescriptorSetLayout handle = VK_NULL_HANDLE;
    uint32_t bindingCount = 0;
    VkDescriptorType types[kMaxBindingsPerSet]{};
};

// Effect: builds the set layout the two stages declare between them. Both are read:
//         vertex asking for a uniform and fragment for a sampler is the usual shape.
bool BuildSetLayout(const VulkanDevice& dev,
                    const ShaderInterface& vert, const ShaderInterface& frag,
                    DescriptorLayout* out) noexcept;

// Effect: reads path and fills out. No device involved.
// Output: false on a missing or malformed .spv, or more than kMaxBindingsPerSet.
bool ReflectShaderFile(const char* path, ShaderInterface* out) noexcept;

// Same file, plus the VkShaderModule. Reads the file once for both.
//
// Output: VK_NULL_HANDLE on failure
VkShaderModule LoadShader(const VulkanDevice& dev, const char* path,
                          ShaderInterface* out) noexcept;
