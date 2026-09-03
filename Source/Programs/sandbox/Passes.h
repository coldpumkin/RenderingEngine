#pragma once

// Passes - what we draw, and in what order
// ============================================================================
//
// Outside Vulkan/ because none of this is the API: a pass is our arrangement of it.
// The dependency runs one way -- this file names Vulkan types, and no header under
// Vulkan/ names a pass.
//
// Two passes. A pass is one render-target configuration with draws in it, inside its
// own BeginRendering scope; how many pipelines those draws use is not fixed at one.
// The second reads what the first wrote, and the order is the two calls in
// RecordFrame -- nothing else enforces it:
//
//   [scene pass]   knows nothing about the window
//     barrier x3 (the pass's color, colorResolve, depth for this slot)
//     BeginRendering   attachment = color / depth, resolving into colorResolve
//       BindVertexBuffers, BindIndexBuffer
//       BindPipeline, BindDescriptorSets
//       per item: PushConstants, DrawIndexed
//     EndRendering    <- the multisample average happens here
//
//   [post-process pass] samples the resolve. Nothing is applied to it yet, but this
//                       is the scope an effect goes in
//     barrier resolve   -> SHADER_READ_ONLY
//     barrier target    -> COLOR_ATTACHMENT
//     BeginRendering   attachment = the frame's target, in its own format
//       BindPipeline, BindDescriptorSets, Draw 3 vertices
//     EndRendering
//     barrier target    -> PRESENT_SRC
//
// Pipeline sits in the middle of three agreements, and a pass owns both sides of each:
//   pipeline <-> render target   attachment format (dynamic rendering bakes it in)
//   pipeline <-> vertex buffer   vertex layout
//   pipeline <-> descriptor set  set layout
//
// No fence, semaphore, acquire or present appears here. That lives in Vulkan/Frame,
// and recording and synchronization do not know about each other.

#include "Config.h"   // kFramesInFlight sizes ScenePass::frames
#include "Vulkan/Attachments.h"
#include "Vulkan/Buffer.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Texture.h"

#include <glm/glm.hpp>   // the shader-facing structs hold matrices

struct Mesh;


// Which set is which
// ============================================================================
//
// The shaders declare positions and Vulkan/ reports them; the names are here because
// what a set holds is this layer's decision. Set numbers are also a binding cost:
// vkCmdBindDescriptorSets rebinds from the first changed set upward, so the one that
// changes least often goes first.
constexpr uint32_t kFrameSet = 0;      // camera and light. One per frame in flight
constexpr uint32_t kMaterialSet = 1;   // what a surface looks like. One per material


// What the shaders read
// ============================================================================
//
// Here rather than in Vulkan/Pipeline.h because the fields answer to mesh.vert and
// mesh.frag, not to the API. The pipeline layer only needs their sizes, and it gets
// those out of the .spv.

// Contract: field order and types match the shader's Scene block. Written once per
//           frame; every draw in the pass reads the same values, so what differs per
//           draw goes in PushConstants instead.
//
// vec4 rather than vec3: std140 aligns a vec3 to 16 bytes anyway, so naming the
// leftover beats hiding it.
struct SceneUniform {
    glm::mat4 viewProj;
    glm::vec4 lightDir;     // xyz = surface toward the light, w unused
    glm::vec4 lightColor;   // rgb = colour, a = ambient
    glm::vec4 viewPos;      // xyz = camera position, w = specular exponent
};

// Rides inside the command buffer: no pool, no set, no lifetime. At least 128 bytes
// are guaranteed, which is why the three matrices are multiplied on the CPU - sent
// apart they would be 192. Lighting that wants world space splits model back out.
//
// Contract: field order and types match the shader's push_constant block. The layer
//           checks the size, not the order.
// Contract: every stage that reads it must be in pushRange.stageFlags - fragment
//           reads alpha, so VERTEX alone is not enough.
struct PushConstants {
    glm::mat4 mvp;   // model -> world -> view -> clip
    float alpha;     // 1.0 is opaque. Opaque pipelines ignore it: blending is off

    // glTF alphaMode MASK: a texel below this is thrown away. 0 keeps everything,
    // which is what OPAQUE means, so the two modes are one value and not a flag
    // beside it -- a flag would make "masked, cutoff 0" expressible.
    //
    // Here rather than in the material set because the asset has exactly one cutoff
    // (0.5, on all three of its masked materials). A set would be a second thing to
    // bind for a number that never differs.
    float alphaCutoff;
};


// Material - what a surface looks like, apart from where it is
// ============================================================================
//
// One set, drawn from the pool and filled once. The texture is not owned here: how
// many textures a scene has is the scene's business, and two materials naming the
// same image is normal.
//
// Two bindings now, in the one set. The prediction written here held: a second thing
// a material owns is another binding, not another set, because it is counted the same
// way -- one per material. Roughness would be the third.
struct Material {
    VkDescriptorSet set = VK_NULL_HANDLE;
};

// What one material is made of, before it becomes a set. Pointers: the scene owns the
// textures, and two materials naming one image share it.
//
// Neither may be null. A material the asset left without a normal map takes a flat
// one, which is the caller's to supply -- this layer has no way to make a texture.
struct MaterialTextures {
    const Texture* baseColor = nullptr;
    const Texture* normal = nullptr;
};

// Effect: draws one set per material and points each at its textures
//
// Contract: pipeline must be the one these will be bound with -- the set is drawn
//           from its material layout.
bool CreateMaterials(const Descriptors& descriptors, const Pipeline& pipeline,
                     const MaterialTextures* sources, uint32_t count,
                     Material* out) noexcept;


// What one draw is
// ============================================================================

// A span inside the index buffer.
//
// Holding the two numbers together lets DrawItem carry a name instead of a position.
struct IndexRange {
    uint32_t firstIndex = 0;
    uint32_t count = 0;

    // Next span starts here, so spans chain instead of repeating positions.
    constexpr uint32_t End() const noexcept { return firstIndex + count; }
};

// What differs between draws, once the pass has fixed everything else.
//
// The material arrived here the day a second texture did. Pipeline and camera have
// not: there is still one of each, and they move in the same way when there are two.
struct DrawItem {
    glm::mat4 model{1.0f};
    float alpha = 1.0f;
    float alphaCutoff = 0.0f;   // 0 = draw every texel
    IndexRange range{};

    // Which pipeline draws this. The asset decides it: glTF doubleSided means
    // cullMode NONE, and cull is baked into a pipeline, so a shader cannot switch it.
    //
    // Contract: every pipeline named here must share the pass's pipeline layout --
    //           they are built from the same shaders, so they do.
    const Pipeline* pipeline = nullptr;

    // The set, not an index into a list the recorder would also have to be handed.
    // Bound only when it differs from the last one, so the order items are written in
    // decides how many binds happen -- that is what a sort key would be sorting.
    // The pipeline above is bound the same way, and the two do not change together.
    VkDescriptorSet material = VK_NULL_HANDLE;

    // Added to every index this draw reads, so a primitive's indices can stay
    // relative to its own vertices. glTF numbers each primitive from zero, and
    // Sponza has 192,496 vertices across 103 of them -- without this the indices
    // would have to be rewritten into uint32 while merging.
    int32_t vertexOffset = 0;
};


// ScenePass - the off-screen pass, and what it draws into
// ============================================================================
//
// The pass is one; its attachments are one set per frame in flight. Every frame
// draws into them again, so a frame cannot share them with one the GPU has not
// finished -- the first barrier in recording is srcStage TOP_OF_PIPE, which waits
// for nothing.
//
// The mesh sits beside frames[], not inside it: nothing writes it after creation, so
// every frame reads the same one. A pointer because the scene owns it.
//
// No texture here any more. It was one because there was one, and the moment a second
// arrived it stopped being a property of the pass -- it is a Material now, and the
// DrawItem says which.
struct ScenePass {
    const Mesh* mesh = nullptr;

    // The draws bring their own now; this one is what the pass itself needs -- the
    // layout to bind set 0 with, and the viewport sign. Both are the same across
    // every pipeline a draw can name, because they all come from these shaders.
    //
    // It is still here because the pass owns both sides of a pair: the pipeline bakes
    // in the attachment formats, and frames[].color is made from the same ones.
    const Pipeline* pipeline = nullptr;

    struct PerFrame {
        Texture color;         // multisample. Drawn into, then discarded
        Texture colorResolve;  // 1 sample. vkCmdEndRendering averages into it, and
                               // the post pass samples it -- the one that leaves
        Texture depth;         // multisample. Tested and written, never read outside

        // The value and its GPU copy, paired the way Texture pairs desc and image.
        // Per frame for the other reason the attachments are: the CPU writes this one
        // while the GPU still reads the previous frame's.
        SceneUniform uniformValue{};
        Buffer uniform;

        // Drawn from the pool by this pass and filled by it: the set names this
        // frame's input and uniform, so no one else knows what belongs in it.
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    PerFrame frames[kFramesInFlight];
};

// Effect: creates each frame's attachments and uniform buffer, and points the pass at
//         what the scene brings.
//
// Contract: formats must be what pipeline was built with. Both are arguments here so
//           the mismatch is at least in one call, but nothing checks it.
bool CreateScenePass(const VulkanDevice& dev, const Descriptors& descriptors,
                     AttachmentFormats formats, VkExtent2D extent,
                     const Mesh& mesh,
                     const Pipeline& pipeline, ScenePass* out) noexcept;


// PostProcessPass - reads what the scene pass produced, writes the frame's target
// ============================================================================
//
// source is a dependency, not an order. Holding the pointer does not stop anyone from
// recording this pass first; the order is the two lines in RecordFrame and stays
// there. Passes ordered by the CPU is the point -- there is no graph to walk.
//
// No attachments of its own: what it draws into arrives with the frame, sized by the
// swapchain's image count rather than by frames in flight.
struct PostProcessPass {
    const ScenePass* source = nullptr;
    const Pipeline* pipeline = nullptr;   // non-owning

    // One per frame in flight, because each names that frame's colorResolve. Flat
    // rather than a PerFrame like the scene pass, since a set is all there is.
    VkDescriptorSet sets[kFramesInFlight]{};
};

// Effect: draws this pass's sets and points each at the matching frame of source
//
// Contract: source must already be created -- the sets name its colorResolve images.
bool CreatePostProcessPass(const Descriptors& descriptors, const ScenePass& source,
                           const Pipeline& pipeline, PostProcessPass* out) noexcept;


// Effect: resets the slot's command buffer and records both passes from it
// Output: false means the buffer is invalid and must not be submitted
//
// Takes the slot but never touches its fence or semaphore -- a rule, not a type.
// A Texture, not the whole FrameTarget: nothing here reads the index or the semaphore,
// and those belong to getting the frame out, not to drawing it.
bool RecordFrame(const FrameSlot& slot, const ScenePass& scene,
                 const PostProcessPass& post, const Texture& target,
                 const DrawItem* items, uint32_t itemCount) noexcept;
