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
//     EndRendering     <- leaves it COLOR_ATTACHMENT_OPTIMAL, and says so
//
//   [gui pass] (Gui.cpp) draws on top with loadOp LOAD
//
//   barrier target -> PRESENT_SRC, in RecordFrame. After every pass, because which
//   one is last is the frame's business and not any pass's
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

// The panel is a pass too, and RecordFrame orders the three. Forward declared rather
// than included: this file names it, Gui.h does not name a pass, and the arrow stays
// pointing one way.
struct Gui;


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
// Here rather than in Vulkan/Pipeline.h because the fields answer to scene.vert and
// scene.frag, not to the API. The pipeline layer only needs their sizes, and it gets
// those out of the .spv.

// Contract: field order and types match the shader's Scene block. Written once per
//           frame; every draw in the pass reads the same values, so what differs per
//           draw goes in PushConstants instead.
//
// Only values this frame computed. The panel's switches used to be here too, and they
// answer to nothing in this struct -- they are the panel's, and binding 2 of this set
// is where they arrive now.
//
// vec4 rather than vec3: std140 aligns a vec3 to 16 bytes anyway, so naming the
// leftover beats hiding it.
struct SceneUniform {
    glm::mat4 viewProj;

    // The same world from the light's side, which is what turns a depth in the shadow
    // map into a comparison with this fragment. One per frame like the camera: every
    // draw in the pass reads it, and it changes when the light moves.
    glm::mat4 lightViewProj;

    glm::vec4 lightDir;     // xyz = surface toward the light, w unused
    glm::vec4 lightColor;   // rgb = colour, a = ambient
    glm::vec4 viewPos;      // xyz = camera position, w = specular exponent


};

// Rides inside the command buffer: no pool, no set, no lifetime. The spec guarantees
// only 128 bytes, so what goes here is what changes per draw and nothing else.
//
// model, not an mvp. It was one when the camera was a per-draw value; the camera
// moved into the frame's uniform, and the vertex shader multiplies viewProj there.
// Sending both would be 128 bytes of matrices alone.
//
// Contract: field order and types match the shader's push_constant block. The layer
//           checks the size, not the order.
// Contract: every stage that reads it must be in pushRange.stageFlags - fragment
//           reads alpha, so VERTEX alone is not enough.
struct PushConstants {
    glm::mat4 model;   // object -> world. viewProj is in SceneUniform

    // transpose(inverse(mat3(model))), one column per vec4. A normal is a covector:
    // it does not transform by the model matrix, and under non-uniform scale the two
    // answers differ. Computed on the CPU because a 3x3 inverse per vertex would pay
    // 192,496 times a frame for a value that changes when the object moves.
    //
    // vec4 rather than a mat3: GLSL pads a mat3's columns to 16 bytes and glm::mat3
    // does not, so the two would disagree by 12 bytes with nothing to say so.
    // Naming the leftover is the same choice SceneUniform makes.
    //
    // The tangent does not use this. It is a direction along the surface, so it takes
    // the model matrix -- the two rules only coincide while the scale is uniform.
    glm::vec4 normal[3];

    float alpha;       // 1.0 is opaque. Opaque pipelines ignore it: blending is off
};
// 116 of the 128 bytes the spec guarantees. Splitting model out is the trigger written
// in CLAUDE.md, and this is most of why there is one.
static_assert(sizeof(PushConstants) == 116, "push constant block grew past its layout");

// The material's numbers, as the shader reads them. One per material, in set 1
// beside its images.
//
// Here rather than in PushConstants because it is counted by materials and that block
// is counted by draws -- one block, one rate, and the faster of the two wins.
//
// Contract: field order and types match the shader's MaterialBlock. std140 rounds a
//           block up to 16 bytes, so the leftover is named rather than hidden.
struct MaterialParams {
    glm::vec4 baseColorFactor{1.0f};   // rgb multiplies the texture, a its alpha
    float alphaCutoff = 0.0f;          // 0 keeps every texel
    float pad[3]{};
};


// Material - what a surface looks like, apart from where it is
// ============================================================================
//
// One set, drawn from the pool and filled once. The texture is not owned here: how
// many textures a scene has is the scene's business, and two materials naming the
// same image is normal.
//
// Three bindings, in the one set. The prediction written here held twice: a second and
// a third thing a material owns are more bindings, not more sets, because they are
// counted the same way -- one per material.
//
// The set is not all of it. What a surface looks like also decides one thing no shader
// can be handed, and that is the line the last field is on.
struct Material {
    VkDescriptorSet set = VK_NULL_HANDLE;

    // Binding 2 of that set. Owned rather than borrowed, unlike the textures: an image
    // can be shared between materials, these numbers are one per material by
    // definition.
    Buffer params;

    // glTF doubleSided, as the value the API wants. Here rather than on the draw
    // because it is counted the way the set is -- one per material, never per draw --
    // and a draw storing it again lets the two disagree.
    //
    // Not a binding: the rasterizer is not a shader input, so this is the one material
    // value that cannot ride in the set. It goes out as vkCmdSetCullMode instead.
    //
    // A function of the material only because the loader keys materials on it. Two
    // glTF materials sharing textures but differing here stay two.
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
};

// What one material is made of, before it becomes a Material. Pointers: the scene owns
// the textures, and two materials naming one image share it.
//
// Neither texture may be null. A material the asset left without a normal map takes a
// flat one, which is the caller's to supply -- this layer has no way to make a texture.
struct MaterialDesc {
    const Texture* baseColor = nullptr;
    const Texture* normal = nullptr;
    MaterialParams params;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
};

// Effect: draws one set per material and points each at its textures
//
// Contract: pipeline must be the one these will be bound with -- the set is drawn
//           from its material layout.
bool CreateMaterials(const VulkanDevice& dev,
                     const Descriptors& descriptors, const DescriptorLayout& layout,
                     const MaterialDesc* sources, uint32_t count,
                     Material* out) noexcept;


// What one draw is
// ============================================================================

// No material. A DrawItem's index defaults to this, so an item nobody assigned one to
// is not silently the material that happens to sit at 0.
//
// UINT32_MAX and not -1: it is also above every valid index, so one comparison against
// the array's size catches both an unset item and an index past the end.
constexpr uint32_t kNoMaterial = UINT32_MAX;

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
// The material arrived here the day a second texture did. The camera has not: there
// is still one, and it moves in the same way when there are two.
struct DrawItem {
    // Set together through SetDrawModel: normal is derived from model and the two must
    // not be written apart.
    glm::mat4 model{1.0f};
    glm::vec4 normal[3]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

    float alpha = 1.0f;
    IndexRange range{};

    // Where the material is, not where it lives in memory. Materials sit end to end in
    // one array and the position is what identifies one; a pointer spelled that
    // position as an address, which cost three things. It ordered by an address nobody
    // chose, it was too wide to pack into a sort key, and "do not resize the array" was
    // a comment rather than something the type survives.
    //
    // Bound only when it differs from the last one, so the order items are written in
    // decides how many binds happen -- that is what a sort key sorts. Cull is set the
    // same way and changes with it, because both are read through this one index.
    uint32_t material = kNoMaterial;

    // Added to every index this draw reads, so a primitive's indices can stay
    // relative to its own vertices. glTF numbers each primitive from zero, and
    // Sponza has 192,496 vertices across 103 of them -- without this the indices
    // would have to be rewritten into uint32 while merging.
    int32_t vertexOffset = 0;
};

// Effect: sets a draw's model matrix and the normal matrix that goes with it.
//
// One call because the two are one fact. Setting model alone leaves normals answering
// to the previous transform, which is invisible until a scale is not uniform and
// silently wrong after that.
void SetDrawModel(DrawItem* item, const glm::mat4& model) noexcept;


// What a pass is asked to draw
// ============================================================================
//
// One type because an index means nothing without the array it indexes. Kept apart,
// the two could be handed in from different places and disagree, and nothing would
// say so -- the draw would simply bind another material's set.
//
// Passed per recording rather than owned by the pass, for the reason the item list
// always was: the items are the frame's and the materials are the scene's. What
// changed is that they now have to arrive together.
struct DrawList {
    const DrawItem* items = nullptr;
    uint32_t itemCount = 0;

    // What item.material indexes. materialCount is read, not decoration: it is the
    // bound every index is checked against.
    const Material* materials = nullptr;
    uint32_t materialCount = 0;
};


// ShadowPass - the same surfaces, depth only, from where the light is
// ============================================================================
//
// The first pass here with no colour attachment. Its product is a depth image the
// scene pass samples, which makes it also the first thing depth does outside the
// frame that produced it.
//
// One matrix, its own set, its own program. Not the scene's uniform: that one carries
// a camera and four switches this stage never reads, and one layout answering to half
// of each pass is how a set stops meaning anything.
//
// Contract: field order matches the Shadow block in shadow.vert.
struct ShadowUniform {
    glm::mat4 lightViewProj;
};

struct ShadowPass {
    const Mesh* mesh = nullptr;
    const ShaderProgram* program = nullptr;
    const Pipeline* pipeline = nullptr;

    // Per frame in flight for the reason the scene's attachments are: the CPU writes
    // the next frame's matrix while the GPU still reads the previous frame's map.
    struct PerFrame {
        // DEPTH_STENCIL_ATTACHMENT to draw into and SAMPLED to be read afterwards.
        // One sample: multisampling a visibility test would average depths that were
        // never on the same surface.
        Texture depth;

        ShadowUniform uniformValue{};
        Buffer uniform;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    PerFrame frames[kFramesInFlight];
};

// Effect: creates each frame's depth map and the set naming its matrix
//
// The formats come out of pipeline. They used to be an argument beside it with a
// contract saying the two must agree and nothing checking it; a pipeline already
// carries what it was compiled for, so the second copy was only a way to disagree.
//
// Contract: extent is square, because the light's box is.
bool CreateShadowPass(const VulkanDevice& dev, const Descriptors& descriptors,
                      VkExtent2D extent,
                      const Mesh& mesh, const ShaderProgram& program,
                      const Pipeline& pipeline, ShadowPass* out) noexcept;


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

    // The shader interface every draw in this pass answers to: set layouts, push
    // range, pipeline layout. It is the pass's and not a pipeline's, because a pass
    // may hold several pipelines and they all bind through this one.
    //
    // What Vulkan requires of a draw here is only that its pipeline was compiled for
    // these attachment formats. Sharing a program is our restriction, not the API's:
    // recording binds set 0 and pushes constants through this one layout, so a
    // pipeline from a different program would have to bring its own -- and every draw
    // would then carry which layout to use.
    //
    // That is the shape a second program in one pass would take, and it is why
    // "one program per pass" is written here rather than assumed: the day a draw needs
    // a different set layout against the same attachments, this field becomes the
    // draw's rather than the pass's.
    const ShaderProgram* program = nullptr;

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
// What the attachments are made of is read off pipeline, the same way the shadow pass
// reads its own. Which images exist at all follows from the same three values: a
// colour format that is not UNDEFINED means a colour attachment, more than one sample
// means a resolve beside it, a depth format means depth. That rule produces this
// pass's three and the shadow pass's one, which is why neither takes them as an
// argument any more.
//
// Contract: shadow must already be created -- each frame's set names its depth map,
//           frame for frame. Taken by value at set-fill time and not stored: the
//           barrier that makes it readable belongs to the pass that writes it.
// Contract: gui must already be created, for the same reason: binding 2 of each set
//           names the buffer its checkboxes write into.
bool CreateScenePass(const VulkanDevice& dev, const Descriptors& descriptors,
                     VkExtent2D extent,
                     const Mesh& mesh, const ShaderProgram& program,
                     const Pipeline& pipeline, const ShadowPass& shadow,
                     const Gui& gui, ScenePass* out) noexcept;


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
    const ShaderProgram* program = nullptr;   // the interface, shared. non-owning
    const Pipeline* pipeline = nullptr;       // the one variant. non-owning

    // One per frame in flight, because each names that frame's colorResolve. Flat
    // rather than a PerFrame like the scene pass, since a set is all there is.
    VkDescriptorSet sets[kFramesInFlight]{};
};

// Effect: draws this pass's sets and points each at the matching frame of source
//
// Contract: source must already be created -- the sets name its colorResolve images.
bool CreatePostProcessPass(const Descriptors& descriptors, const ScenePass& source,
                           const ShaderProgram& program,
                           const Pipeline& pipeline, PostProcessPass* out) noexcept;


// What recording one scene pass cost in state changes.
//
// Counted where it happens rather than worked out from the item list, so the number is
// what the command buffer actually got. These are what a sort order changes: the draws
// are fixed, the other two are not.
//
// They do not fall together. materialBinds reaches its floor -- the number of distinct
// materials -- as soon as equal materials are adjacent. cullChanges reaches its floor
// only if the order groups by cull first, which sorting on the material alone does not
// do even though cull is a function of it.
struct DrawStats {
    uint32_t draws = 0;
    uint32_t materialBinds = 0;
    uint32_t cullChanges = 0;
};

// Effect: resets the slot's command buffer and records both passes from it
// Output: false means the buffer is invalid and must not be submitted
//         stats, if given, is what the scene pass cost. Every frame records the same
//         list, so one frame's numbers are the answer.
//
// Takes the slot but never touches its fence or semaphore -- a rule, not a type.
// A Texture, not the whole FrameTarget: nothing here reads the index or the semaphore,
// and those belong to getting the frame out, not to drawing it.
bool RecordFrame(const FrameSlot& slot, const ShadowPass& shadow,
                 const ScenePass& scene,
                 const PostProcessPass& post, Gui& gui, const Texture& target,
                 const DrawList& draws, DrawStats* stats = nullptr) noexcept;
