#pragma once

// Passes - what the passes share, and the frame that orders them
// ============================================================================
//
// Outside Vulkan/ because none of this is the API: a pass is our arrangement of it.
// The dependency runs one way -- this file names Vulkan types, and no header under
// Vulkan/ names a pass.
//
// A pass is one render-target configuration with draws in it, inside its own
// BeginRendering scope. Each has its own pair of files and is named here, not
// included; what is left in this one is what more than one of them needs:
//
//   ShadowPass.h        depth only, from where the light is
//   ScenePass.h         the surfaces, off-screen, multisampled
//   PostProcessPass.h   the scene's resolve onto the frame's target
//   Gui.h               the panel, drawn on top
//
// The order is the calls in RecordFrame and nothing else enforces it. Every edge
// between two passes is a value main hands to both -- the shadow maps, the resolve --
// so no pass reaches into another to find what it reads:
//
//   shadow -> scene    the map, sampled
//   scene  -> post     the resolve, sampled
//   post   -> gui      the target, drawn on top with loadOp LOAD
//   gui    -> scene    the panel's option buffer, asked for by name. The one edge
//                      that runs backwards, and the only one a pass still owns
//
// The barriers for the first two are inside the pass that wrote the image; the one
// for the third is in RecordFrame beside the present transition. That disagreement is
// open, not settled.
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

// A pass names this file; this file names no pass. Forward declared rather than
// included, so the arrow points one way -- RecordFrame is handed each of them and
// orders them, and none of them knows the others.
struct ShadowPass;
struct ScenePass;
struct PostProcessPass;
struct Gui;

// Named by DrawList below, which holds a pointer to an array of them. What one is
// belongs to the pass that binds it.
struct Material;
struct DrawStats;


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

// Two matrices and not their product
// ----------------------------------------------------------------------------
//
// view is where the camera is; proj is what the target it lands on does to what the
// camera sees. They answer to different things -- view to the keyboard, proj to
// renderExtent -- and multiplying them on the CPU hid that: a render target's extent
// is read by exactly one thing in this program, the projection, and while the product
// was all that reached the GPU there was nothing on that side an extent belonged to.
//
// It also gives a lighting pass its inverses. Reconstructing a world position from a
// depth buffer needs proj undone and then view undone; from the product only the pair
// can be undone at once, which costs the camera-space step every such pass wants.
//
// Contract: field order and types match the shader's Camera block, and viewPos sits at
//           128 -- scene.frag names that offset rather than declaring two matrices it
//           never touches.
struct CameraUniform {
    glm::mat4 view;
    glm::mat4 proj;

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

// Contract: field order and types match the shader's Shadow block, and both stages
//           that read it declare both fields.
struct ShadowUniform {
    // The same world from the light's side, which is what turns a depth in the shadow
    // map into a comparison with this fragment. The shadow pass draws the map with
    // these and the scene pass compares against them, which is why one buffer rather
    // than two: they cannot disagree about values there is one of.
    //
    // Split for the reason the camera's are, and here the two halves are further
    // apart: lightView turns with the clock every frame, lightProj is a box built once
    // out of kShadowRadius and the map's shape. Their product hid a per-frame value
    // and a constant behind one name.
    glm::mat4 lightView;
    glm::mat4 lightProj;
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


// Output: everything the depth targets do between them
//
// Both take the one format QueryTargetCapabilities finds, so what it has to search
// for is the union rather than either one. Read out of the two calls above instead of
// written again here: the scene's depth is drawn into, the shadow map is also
// sampled, and moving a bit in either place moves this.
VkImageUsageFlags DepthTargetUsage() noexcept;


// Effect: copies this frame's values into the buffers the sets already name
//
// The value and its GPU copy meet here, and nothing else in a frame does this: the
// sets were filled once at creation and point at these buffers for good, so a frame's
// work is a memcpy per value and no descriptor write at all.
//
// Apart from RecordFrame because it is not recording -- it touches no command buffer,
// issues no barrier, and the three arrays it needs were three of that function's
// eleven arguments purely to reach these four lines.
//
// Contract: the caller has waited on this slot's fence. BeginFrame does, and this
//           runs after it -- writing a buffer the GPU is still reading is the one way
//           to get this wrong, and nothing here would notice.
void UploadFrameValues(const FrameSlot& slot,
                       const FrameCamera* cameras, const FrameLight* lights,
                       const FrameShadow* shadows, Gui& gui) noexcept;

// Effect: resets the slot's command buffer and records the four passes into it
// Output: false means the buffer is invalid and must not be submitted
//         stats, if given, is what the scene pass cost. Every frame records the same
//         list, so one frame's numbers are the answer.
//
// Takes the slot but never touches its fence or semaphore -- a rule, not a type.
// A Texture, not the whole FrameTarget: nothing here reads the index or the semaphore,
// and those belong to getting the frame out, not to drawing it.
//
// Contract: UploadFrameValues has run for this slot. What is recorded here reads
//           those buffers, and nothing in the command stream would say they are stale.
bool RecordFrame(const FrameSlot& slot,
                 const ShadowPass& shadow, const ScenePass& scene,
                 const PostProcessPass& post, Gui& gui, const Texture& target,
                 const DrawList& draws, DrawStats* stats = nullptr) noexcept;
