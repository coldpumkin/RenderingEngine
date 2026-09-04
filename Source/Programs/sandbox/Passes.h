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

// Two subjects, not one
// ----------------------------------------------------------------------------
//
// These were a single struct called SceneUniform, on the grounds that both are the
// frame's and every draw in the pass reads them. That is true and it is not enough:
// reading the fields one at a time says they are two things.
//
//   camera   viewProj, viewPos          changed by the keyboard      read by 1 pass
//   light    lightViewProj, dir, color  changed by the clock         read by 2
//
// Different reasons to change, and a different number of readers -- two of the three
// grounds for splitting. The third does not hold: both are written once per frame.
//
// The duplicate was the symptom. lightViewProj is in ShadowUniform as well, because a
// value with two readers was living in a struct shaped for one; sharing one buffer is
// the step this makes possible and not the step this is.
//
// It also lets each stage stop declaring what it does not read. In one block the
// fragment stage had to name viewProj -- which it uses nowhere -- to reach past it.
//
// vec4 rather than vec3: std140 aligns a vec3 to 16 bytes anyway, so naming the
// leftover beats hiding it.

// A camera, kept the way a Texture is
// ----------------------------------------------------------------------------
//
// Texture keeps the TextureDesc it was made from, and that is the only reason
// ReadTexturePixels can refuse a format it cannot read: the description outlives the
// making. No matrix here kept anything. proj was built from a field of view, a near
// and a far plane and an aspect, and all four vanished into the product, so nothing
// downstream could ask what shape of target it was built for.
//
// That question came up three times and was answered three different ways -- a
// Contract comment on the shadow pass, a shared name for the scene, LetterboxInto for
// the post pass. All three are the same comparison, and none of them could be a
// comparison, because one side of it had been multiplied away.
//
// So: the same shape as Texture. What it was made from, then what was made.
struct CameraDesc {
    // Where the aspect comes from. The projection answers to the image it lands on,
    // and this is that image's size.
    VkExtent2D target{};

    float fovDegrees = 0.0f;
    float nearPlane = 0.0f;
    float farPlane = 0.0f;

    glm::vec3 eye{};
    glm::vec3 forward{};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

struct Camera {
    CameraDesc desc;
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
};

// Output: the aspect the projection is built with, from the target it lands on
constexpr float CameraAspect(const CameraDesc& desc) noexcept {
    return static_cast<float>(desc.target.width)
         / static_cast<float>(desc.target.height);
}

// Output: both matrices, from the desc that makes them
//
// Built every frame, which the 09-01 note about proj is not an argument against: that
// value was rebuilt in a loop while nothing said what it depended on. Here the
// dependency is the desc, and remaking from it is what keeps proj and target from
// being two variables that have to be updated in step.
Camera MakeCamera(const CameraDesc& desc) noexcept;

// Contract: field order and types match the shader's Camera block, and viewPos sits at
//           64 -- scene.frag names that offset rather than declaring the matrix.
struct CameraUniform {
    glm::mat4 viewProj;

    // w is unused. It carried one specular exponent for the whole scene until
    // roughness came out of the material, which is the value that replaced it.
    glm::vec4 viewPos;      // xyz = camera position
};

// A light and the technique that shadows it are two things
// ----------------------------------------------------------------------------
//
// These were one block, on the grounds that both are the light's. Counted in
// scene.frag they are not: lightViewProj appears **once**, inside ShadowFactor, and
// never in the lighting. What arrives at a surface is a direction and a colour, and
// `dot(normal, toLight)` uses no matrix at all.
//
// So the light is a viewpoint only as far as shadow mapping makes it one. Ray-traced
// shadows would delete the matrix and leave the other two untouched -- which is not
// true of the camera, whose projection is what drawing is. The panel already shows
// the asymmetry: turn shadows off and lightViewProj is dead while direction and
// colour are not.
//
// Two of the three grounds for splitting, the same score as the splits already made
// today. Readers differ -- the matrix is read by the shadow pass and the scene pass,
// the other two only by the scene pass -- and so do the reasons to change: the matrix
// answers to kShadowRadius and the map's shape as well as to where the light points.
// The third does not hold; both are written once a frame.
//
// **And this is not built for a second light.** It is a statement of one removed. The
// day there are N lights and M of them cast, those are different numbers, and a block
// holding both would have to be taken apart first.

// Contract: field order and types match the shader's Shadow block. One field, and the
//           whole of what shadow.vert declares -- there is nothing to truncate now.
struct ShadowUniform {
    // The same world from the light's side, which is what turns a depth in the shadow
    // map into a comparison with this fragment. The shadow pass draws the map with
    // this matrix and the scene pass compares against it, which is why one buffer
    // rather than two: they cannot disagree about a value there is one of.
    glm::mat4 lightViewProj;
};

// Contract: field order and types match the shader's Light block.
struct LightUniform {
    glm::vec4 direction;   // xyz = surface toward the light, w unused
    glm::vec4 color;       // rgb = colour, a = ambient
};

// What a frame computes, once per frame in flight, owned by no pass
// ----------------------------------------------------------------------------
//
// Both of these are written by main every frame and read by a pass that never writes
// them. That is what puts them here rather than inside a pass: a value whose producer
// is outside is one the producer should hold, and the alternative is main reaching
// three levels into a pass to assign it.
//
// The light has a second reason -- two passes read it. It was two buffers once, with
// main assigning the same matrix into both, one value in two homes held together by
// nothing but the local it came from. The camera has only the first reason and that
// was enough: inside ScenePass::PerFrame it shared a struct with attachments it moves
// with in no way at all, one created once and the other rewritten every frame.
//
// Creation order settles that they cannot live in a pass anyway. The shadow pass is
// created first because the scene pass's sets name its maps, so a buffer owned by
// either would have to exist before its owner did.
//
// **Per frame in flight, and only half of each of these needs to be.** The buffer does
// -- the GPU still reads the previous frame's. The value beside it does not: main
// writes it and RecordFrame copies it out in the same turn of the loop, so it is born
// and spent without ever crossing a frame boundary. Kept per frame because separating
// them buys 176 bytes and costs RecordFrame two more arguments; the day the value is
// large (an array of lights) or recording moves off this thread, that is the trade
// changing rather than a new idea.
//
// Three structs and not one template. What repeats is a pattern -- a value and its
// GPU copy -- already on its fourth instance counting Texture's desc and image.
// Naming it would turn FrameCamera into FrameUniform<CameraUniform>, a name traded
// for a type argument, and all four have names worth keeping. A fifth with no name of
// its own would be the thing that changes that.
struct FrameCamera {
    CameraUniform value{};
    Buffer buffer;
};

struct FrameLight {
    LightUniform value{};
    Buffer buffer;
};

// Read by two passes, which is why it outlives both of them here rather than living
// in the one that draws with it.
struct FrameShadow {
    ShadowUniform value{};
    Buffer buffer;
};

// Effect: creates one mapped uniform buffer per frame in flight
bool CreateFrameCameras(const VulkanDevice& dev, FrameCamera* out) noexcept;
bool CreateFrameLights(const VulkanDevice& dev, FrameLight* out) noexcept;
bool CreateFrameShadows(const VulkanDevice& dev, FrameShadow* out) noexcept;

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

    // Both multiply the texture the way baseColorFactor does, and glTF defaults both
    // to 1: fully metallic and fully rough, which is what a material naming neither a
    // texture nor a factor asks for.
    float metallic = 1.0f;
    float roughness = 1.0f;

    float pad{};   // std140 rounds the block to 32
};


// Material - what a surface looks like, apart from where it is
// ============================================================================
//
// One set, drawn from the pool and filled once. The texture is not owned here: how
// many textures a scene has is the scene's business, and two materials naming the
// same image is normal.
//
// Four bindings, in the one set. The prediction written here has held three times now:
// each new thing a material owns is another binding, not another set, because they are
// all counted the same way -- one per material.
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

    // glTF packs two values into one image: green is roughness, blue is metallic.
    // Red is free and some tools put occlusion there, which we do not read.
    const Texture* metallicRoughness = nullptr;

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


// What a pass draws into, described the way every other texture here is
// ============================================================================
//
// A TextureDesc says four things and AttachmentFormats says two of them, so these are
// what a target actually is and the pipeline's value is derived from them. The two
// AttachmentFormats drops are the two that mattered all along:
//
//   extent   the aspect a projection is built with comes from here
//   usage    ATTACHMENT is what the pipeline draws into. **SAMPLED is an edge** --
//            it marks the images another pass reads, and there are exactly two
//
// Written by the caller, beside the other things it hands a pass, rather than made up
// inside pass creation from formats read back off a pipeline.
struct SceneTargetDescs {
    TextureDesc color;     // multisample. Drawn into, then discarded
    TextureDesc resolve;   // 1 sample. What leaves the pass
    TextureDesc depth;     // multisample. Never read outside the frame
};

// Output: the three, from one size and the formats
//
// The expansion rule, which used to be four lines inside CreateScenePass and a
// sentence in its comment. Two callers now -- creation and every resize.
SceneTargetDescs MakeSceneTargets(VkExtent2D extent, VkFormat colour, VkFormat depth,
                                  VkSampleCountFlagBits samples) noexcept;

// Output: the one image the shadow pass makes
//
// One sample, always: averaging depths across an edge produces a value no surface was
// ever at, and every fragment comparing against it is wrong. SAMPLED because the scene
// pass reads it -- the second of the two edges.
TextureDesc MakeShadowTarget(VkExtent2D extent, VkFormat depth) noexcept;

// Output: what a pipeline drawing into these is compiled against
//
// The projection Attachments.h describes, as a call. main wrote it out field by field
// before, once per pass, so the value a pipeline baked and the value its images were
// made from came from two hands -- and the claim that they cannot disagree held only
// because nobody mistyped it. Now the pass creations call the same one to check.
//
// Named the way SwapchainAttachmentFormats is: one per thing that can answer, and the
// name says which is answering.
AttachmentFormats SceneAttachmentFormats(const SceneTargetDescs& targets) noexcept;
AttachmentFormats ShadowAttachmentFormats(const TextureDesc& map) noexcept;


// ShadowPass - the same surfaces, depth only, from where the light is
// ============================================================================
//
// The first pass here with no colour attachment. Its product is a depth image the
// scene pass samples, which makes it also the first thing depth does outside the
// frame that produced it.
//
// Its own set and its own program, but not its own light: the matrix it draws with is
// FrameLight's, handed in. What stays the pass's is the map.
struct ShadowPass {
    const Mesh* mesh = nullptr;
    const ShaderProgram* program = nullptr;
    const Pipeline* pipeline = nullptr;

    // Per frame in flight for the reason the scene's attachments are: the GPU still
    // reads the previous frame's map while the next is drawn.
    struct PerFrame {
        // DEPTH_STENCIL_ATTACHMENT to draw into and SAMPLED to be read afterwards.
        // One sample: multisampling a visibility test would average depths that were
        // never on the same surface.
        Texture depth;

        // Names the FrameLight of the same index. No buffer beside it any more.
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
// Contract: mapDesc.extent is square, because the light's box is.
// Contract: shadows holds kFramesInFlight entries and outlives this pass -- each set
//           names the buffer of the same index.
bool CreateShadowPass(const VulkanDevice& dev, const Descriptors& descriptors,
                      const TextureDesc& mapDesc,
                      const Mesh& mesh, const ShaderProgram& program,
                      const Pipeline& pipeline, const FrameShadow* shadows,
                      ShadowPass* out) noexcept;


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
//
// What the pass owns, and what reaches it from outside
// ----------------------------------------------------------------------------
//
// Sorted by that question rather than by type, because the answer is lopsided:
//
//   owns       the images it draws into. That is the whole list.
//
//   receives   the light                  an argument, one buffer per frame
//              the shadow maps            an argument, one image per frame
//              the raster switches        an argument to RecordScenePass
//              the camera                 main assigns into frames[i] from outside
//              the panel's buffer         asked for: GuiOptionsBuffer(gui, i)
//
// Three of the five now say what they are in the signature. The camera does not --
// main reaches in and writes it -- and the panel is deliberately different: it is a
// tool for making features comparable while they are understood, not a dependency of
// the same kind, and asking a Gui for its buffer is not the same as reaching into a
// pass for an image.
//
// This is written as a list and not as a type on purpose. Naming what a pass owns is
// what has to happen before anything derives from it, and a struct now would fix the
// answer while three of the four routes still have no reason to be what they are.
//
// The line is also not a partition. "The pass owns this" says nothing about what a
// draw owns, and the tempting reading -- everything else is the draw's -- would settle
// a question the asset is currently answering. The pipeline below is the case: it is
// the pass's because no material in Sponza asks for a second one, not because a pass
// is the thing that holds a pipeline.
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

    // Two variants of the one program, and the pass picks between them at record
    // time. They differ in polygonMode and in nothing else -- same shaders, same set
    // layouts, same push range, same attachment formats.
    //
    // That sameness is the point rather than a coincidence: every descriptor set this
    // pass allocated was drawn from program's layouts, so switching between these two
    // rebinds nothing. It is what "a pipeline is one variant of a program" means when
    // there is finally more than one.
    //
    // Not on the DrawItem. Which pipeline is used is one answer for the whole pass,
    // not something a draw decides -- and this asset gives no reason for it to be:
    // 25 materials, 22 OPAQUE and 3 MASK, and MASK is a discard in the shader.
    const Pipeline* pipeline = nullptr;
    const Pipeline* wirePipeline = nullptr;

    struct PerFrame {
        Texture color;         // multisample. Drawn into, then discarded
        Texture colorResolve;  // 1 sample. vkCmdEndRendering averages into it, and
                               // the post pass samples it -- the one that leaves
        Texture depth;         // multisample. Tested and written, never read outside

        // No uniform buffer here any more. Both of the ones that were -- the camera
        // and, before it, the light -- are written by main and read by this pass, and
        // nothing in this struct moves with them: these three images are made once and
        // never touched again.
        //
        // What is left is what the pass is: images it draws into, and the set that
        // names them beside what it was handed.

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
// shadowMaps and not a ShadowPass, which is the whole of the change: what this
// needs is one depth image per frame, and naming the pass that owns them let this
// function reach anything a shadow pass has. A signature is meant to state the
// requirement, not a place the requirement can be found in.
//
// gui stays a whole Gui on purpose. It is not a dependency of the same kind -- the
// panel exists to make features comparable while they are being understood, and it
// is deliberately a black box to whoever reads main. Its buffer already arrives
// through an accessor, which is asking rather than reaching in.
//
// Contract: shadowMaps holds kFramesInFlight entries, each an image the shadow pass
//           has created, frame for frame. Read at set-fill time and not stored: the
//           barrier that makes one readable belongs to the pass that writes it.
// Contract: gui must already be created -- binding 3 of each set names the buffer its
//           checkboxes write into.
// Effect: remakes every frame's attachments at a new size, leaving the sets alone
//
// The other half of creation, and the reason the three images are made by a function
// rather than written out once: they are made twice now, here and there.
//
// This pass's own sets name nothing that changes -- the camera, the light, a shadow
// map and the panel's buffer -- so they survive. **The post pass's do not**, because
// they name the resolve image this destroys; RefreshPostProcessPass is the other half
// and the caller runs it.
//
// Contract: the GPU must be idle. The caller waits -- a frame in flight is still
//           reading last frame's attachments, and no fence here says which.
bool ResizeScenePass(const VulkanDevice& dev, const SceneTargetDescs& targets,
                     ScenePass* pass) noexcept;

// Contract: cameras, lights and shadows hold kFramesInFlight entries and outlive this
//           pass. shadows is the same array the shadow pass was given, which is what
//           makes the matrix in binding 2 the one that drew the map in binding 3.
bool CreateScenePass(const VulkanDevice& dev, const Descriptors& descriptors,
                     const SceneTargetDescs& targets,
                     const Mesh& mesh, const ShaderProgram& program,
                     const Pipeline& pipeline, const Pipeline& wirePipeline,
                     const Texture* const shadowMaps[kFramesInFlight],
                     const FrameCamera* cameras, const FrameLight* lights,
                     const FrameShadow* shadows,
                     const Gui& gui, ScenePass* out) noexcept;


// PostProcessPass - reads what the scene pass produced, writes the frame's target
// ============================================================================
//
// **It takes the images, not the pass that made them.** Everything this pass needs of
// its input is what a Texture already says -- extent, format, one sample -- and none
// of those three is the scene's to decide. A multisample image cannot be sampled, so
// the resolve exists for this reader; the format has to mean what fullscreen.frag
// assumes of it; and the extent it carries is the aspect the projection was built
// from. The producer answers to the consumer here, which is the other way round from
// how the two are named.
//
// Naming the edge as images is also what lets the caller write it down: main fills the
// array, so the dependency is a value in one place instead of a path walked from in
// here. That is as far as this goes -- what it does not yet do is compare the extent
// it samples with the extent it draws into, which is a contract nothing states.
//
// A dependency, not an order. Holding these pointers does not stop anyone from
// recording this pass first; the order is the two lines in RecordFrame and stays
// there. Passes ordered by the CPU is the point -- there is no graph to walk.
//
// No attachments of its own: what it draws into arrives with the frame, sized by the
// swapchain's image count rather than by frames in flight.
struct PostProcessPass {
    // One per frame in flight. Non-owning: the scene pass owns these images.
    const Texture* source[kFramesInFlight]{};

    const ShaderProgram* program = nullptr;   // the interface, shared. non-owning
    const Pipeline* pipeline = nullptr;       // the one variant. non-owning

    // One per frame in flight, because each names the source above it. Flat rather
    // than a PerFrame like the scene pass, since a set is all there is.
    VkDescriptorSet sets[kFramesInFlight]{};
};

// Effect: rewrites each set to name its source image again
//
// The pointers in source[] do not change when a target is remade -- the Texture stays
// where it is and its contents are replaced -- but the view handle inside does, and a
// set records a handle rather than a pointer. So a resize needs this and nothing else.
void RefreshPostProcessPass(const Descriptors& descriptors,
                            PostProcessPass* post) noexcept;

// Effect: draws this pass's sets and points each at the matching source image
//
// Contract: source[i] is created and outlives this pass, and is 1-sample -- a
//           multisample image cannot be bound to a sampler. Validation says so.
bool CreatePostProcessPass(const Descriptors& descriptors,
                           const Texture* const source[kFramesInFlight],
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
bool RecordFrame(const FrameSlot& slot,
                 const FrameCamera* cameras, const FrameLight* lights,
                 const FrameShadow* shadows,
                 const ShadowPass& shadow, const ScenePass& scene,
                 const PostProcessPass& post, Gui& gui, const Texture& target,
                 const DrawList& draws, DrawStats* stats = nullptr) noexcept;
