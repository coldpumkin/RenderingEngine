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

#include <cstddef>    // offsetof, for the block members MaterialSet declares
#include <iterator>   // std::size

#include <glm/glm.hpp>                // the shader-facing structs hold matrices
#include <glm/gtc/quaternion.hpp>     // Transform holds an orientation

struct Mesh;

// A pass names this file; this file names no pass. Forward declared rather than
// included, so the arrow points one way -- RecordFrame is handed each of them and
// orders them, and none of them knows the others.
struct ShadowPass;
struct SkyPass;
struct ScenePass;
struct GeometryPass;
struct LightingPass;
struct PostProcessPass;
struct Gui;

// Their headers are not included here for the same reason a pass's is not. Out
// parameters need no more than this.
struct SceneTargetDescs;
struct GBufferTargetDescs;



// Which set is which
// ============================================================================
//
// The shaders declare positions and Vulkan/ reports them; the names are here because
// what a set holds is this layer's decision. Set numbers are also a binding cost:
// vkCmdBindDescriptorSets rebinds from the first changed set upward, so the one that
// changes least often goes first.
constexpr uint32_t kFrameSet = 0;      // camera and light. One per frame in flight
constexpr uint32_t kMaterialSet = 1;   // what a surface looks like. One per material

// What a material is, declared here rather than read out of whichever shader happens to
// use one. Set 0 is each program's own -- shadow, scene and post fill it with different
// things -- but set 1 is spoken by every program that draws a surface, and a second one
// of those is what deferred adds.
//
// Declared, a shader's own bindings become a claim this refuses. Left to reflection,
// two shaders would each be right about their own layout and a set filled for one would
// land in the wrong slots of the other -- four samplers in a row are the same shape
// whatever order they are in, so nothing would say a word.
//
// The order is the binding order, and CreateMaterials fills in this order for the same
// reason: one declaration or two.
// (MaterialSet() is below MaterialParams, which it takes its offsets from.)


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

// What the set is made of. What the block inside binding 2 looks like is a separate
// requirement -- SharedBlocks() below -- because the same block appears at other
// bindings in other programs and a slot cannot reach those.
inline RequiredSet MaterialSet() noexcept {
    RequiredSet set;
    set.set = kMaterialSet;
    set.bindingCount = 4;
    set.types[0] = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    set.names[0] = "baseColor";
    set.types[1] = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    set.names[1] = "normalMap";
    set.types[2] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    set.names[2] = "mtl";
    set.types[3] = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    set.names[3] = "metallicRoughnessMap";
    return set;
}



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
// Where something is and which way it is turned
// ----------------------------------------------------------------------------
//
// State, and not a direction derived from it. lookAt does not take an orientation --
// it makes one, by crossing a forward with an up hint, and that manufacture is what
// costs: the up is consumed rather than kept, so roll has nowhere to live, and the
// cross collapses when the two are parallel. main.cpp's pitch clamp is that collapse
// and not a decision about how a camera should move.
//
// No scale. Nothing here has one -- a camera cannot scale, and the draw items share a
// single constant that belongs to the asset's units rather than to any one object. A
// loader that reads glTF nodes brings the third field with it, and stores the same
// quaternion these nodes already hold.
// Which way is up, for everything in this program
//
// Neither the app's nor the renderer's -- a convention both sides already agree on and
// could not disagree about without the picture inverting. ViewportY, the ban on
// proj[1][1] *= -1, and the normal rules all read the same y.
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

// Rigid: rotation and translation, and no scale
//
// Contract: no scale, or ViewFromPose returns a wrong matrix rather than an
//           incomplete one -- its inverse is a rigid transform's and no other's.
struct Pose {
    glm::vec3 position{};

    // w, x, y, z. The identity looks down -z with +y up, which is the convention
    // mat4_cast writes and the one a projection built by GLM expects.
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Output: the view matrix -- the inverse of that pose
//
// Not glm::inverse. A unit quaternion's conjugate is its inverse and a rigid
// transform's is [R^T | -R^T t], so the general cofactor path would spend work
// deriving what is already known -- and it is only known because the input is rigid.
//
// This is the second half of what lookAt does: it builds a basis and then transposes
// it. Only the first half goes away here.
glm::mat4 ViewFromPose(const Pose& pose) noexcept;

// The three glTF stores on a node, in that order. An object's; a camera holds a Pose.
struct Transform {
    glm::vec3 position{};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

// Output: the model matrix, T * R * S
//
// The columns of the rotation scaled and a translation written in, rather than three
// matrix multiplies: the same answer, and it is what T * R * S is once expanded.
glm::mat4 ModelFromTransform(const Transform& transform) noexcept;

// The camera's state, as whatever moves it holds it
//
// State and not a rendering of it: the matrices below are made from this, and nothing
// here is made from them. Two fields and not more -- what is missing is what belongs
// to the renderer, and the split is the point:
//
//   here          where it is, which way it is turned, how wide it sees
//   the renderer  what shape the picture is, and what range becomes depth
struct CameraState {
    // A Pose: a camera has nothing to say with a scale. A uniform one cancels in the
    // perspective divide and moves no pixel; what it would change is what gets
    // clipped, and near, far and the field below already say that.
    Pose pose{};

    // The one projection input that is not the renderer's. "How wide do I want to
    // see" is a choice the thing holding the camera makes; an aspect is a fact about
    // the image, and the planes are a policy (see ProjectionFor).
    float fovDegrees = 0.0f;
};

// The two matrices are two functions, and their inputs do not overlap
// ----------------------------------------------------------------------------
//
// One call making both hid that they answer to different things and change at
// different times:
//
//   view  <- the transform                        every frame, while a key is held
//   proj  <- fov, the target's extent, the planes  when a projection input changes
//
// The second is not "on resize" -- a resize is one case of it, and a field of view
// that moved would be another. The light's projection is already this shape: every
// argument of its ortho is a constant, so it is built once outside the loop.
//
// Neither is stored. A held proj is a second variable that a resize has to remember
// to update, and one perspective() a frame buys the derivation instead.

// Output: the projection, from what shapes it
//
// near and far are not taken. They decide which view-space range becomes depth and
// how the precision spreads across it -- an input to this matrix, not a property of
// the depth attachment, which only says how many bits store the result. They are
// fixed here as the renderer's policy rather than exposed as camera state; the day
// something wants to move them (a zoom, a precision fix) they join CameraState.
glm::mat4 ProjectionFor(float fovDegrees, VkExtent2D target) noexcept;

// No Camera type holding the two matrices beside the state: a caller holds the state,
// calls the two functions and writes the block. The light reads the same way below.

// The light's state, as whatever moves it holds it
//
// A direction where the camera has a Transform, and the asymmetry is the light's own:
// a directional light has no position -- parallel rays have no eye point -- and nothing
// owns a roll about its axis. So the camera hands over a placement and this hands over
// a direction, out of which the shadow technique makes a placement.
//
// Which is why quaternions do not follow here. They were worth it for the camera
// because an orientation was state being derived every frame; there is no orientation
// to preserve in a direction, so a basis has to be manufactured either way.
// What kind of light, which is what decides what one owns
//
// **Not a taxonomy.** The two differ in one thing that changes every layer beneath
// them: a directional light has no position. It is the idealisation of something
// infinitely far away, so it is a property of the world rather than an object in it --
// which is why it never fitted the Transform the camera and the objects use, and why
// the roll about its axis is a value nobody holds.
//
// A spot has a position. That makes it a thing in the scene, and everything downstream
// changes with it: its shadow is a perspective from that point rather than an
// orthographic box around the scene, its light falls off with distance, and it stops
// where its cone does.
enum class LightKind { Directional, Spot, Point };

// How many lights a frame can carry
//
// A ceiling we impose. One shadow map is drawn, for the first light, which is what
// makes the rest cheap enough that four is not a number worth tuning.
inline constexpr uint32_t kMaxLights = 4;

struct LightState {
    LightKind kind = LightKind::Directional;

    // Which way the light travels from, as a surface sees it. Both kinds own this.
    glm::vec3 direction{};

    // Where it is. **Read only when kind is Spot** -- a directional light has no
    // position, and giving it one here would be inventing a fact the world does not
    // have.
    glm::vec3 position{};

    // The cone, as cosines. **Spot only** -- a point light is a positioned light with
    // no cone, so what it sends in a direction does not depend on the direction.
    float innerCos = 0.0f;
    float outerCos = 0.0f;

    // Where the light is called nothing. Positioned kinds only.
    float range = 0.0f;

    glm::vec3 color{1.0f};

    // Not per light. What the sky sends is one fact about the scene, and a second light
    // does not add a second sky.
    float ambient = 0.0f;
};

// Output: the world from the light's side -- what the shadow pass draws the map with,
//         and what the passes that sample it compare against
//
// sceneCenter is neither the light's nor the renderer's: it is what the box has to
// cover, which is a fact about the scene. Taken as an argument rather than held here,
// so "there is one scene and it sits at this point" stays out of the renderer.
//
// The up is chosen inside, next to the cross product it protects. It used to live in
// the frame loop, far from this lookAt, which is how one constant came to hold both a
// lighting decision and this one with only the first written down.
// Output: where the shadow map is drawn from
//
// The two kinds put the eye in different places for different reasons. A directional
// light has none, so one is invented far enough back along the direction to cover the
// scene; a spot has one and it is used.
glm::mat4 ShadowView(const LightState& light, const glm::vec3& sceneCenter) noexcept;

// Output: the box the light sees through
//
// Takes no state at all. How much world the map covers and how far back the light sits
// are the technique's, the aspect is the map's, and nothing of the app's reaches any of
// it -- so this is built once and not per frame, which is the same rule ProjectionFor
// states and the reason both of these read a TextureDesc rather than an extent.
// Output: the projection the shadow map is drawn with
//
// **Orthographic for a directional light and perspective for a spot**, which is the
// same distinction one level down: rays that are parallel against rays that come from
// a point. The map's shape decides the aspect either way.
glm::mat4 ShadowProjectionFor(const LightState& light, VkExtent2D map) noexcept;

// Output: how big to render -- the one of this program's three extents we choose. A
//         surface extent is what the platform answers and a swapchain's must equal
//         it; all three are VkExtent2D and only this one is ours.
VkExtent2D RenderExtentFor(VkExtent2D windowExtent) noexcept;

// Output: what the device can give a render target, asked with our policy
//
// A wrapper over the Vulkan-level query, and the wrapper is the point: the usage and
// the sample count it asks about are this layer's and the layer below should not read
// a policy of ours.
bool RenderTargetCapabilities(const VulkanInstance& inst, VkPhysicalDevice gpu,
                              TargetCapabilities* out) noexcept;

// Output: the two desc families that answer to the render size
//
// One call and one extent for both. They have to agree -- the two chains end in the
// same post pass -- and before this the agreement was main passing one variable twice,
// at startup and again on resize.
void DescribeSizedTargets(VkExtent2D windowExtent, const TargetCapabilities& caps,
                          SceneTargetDescs* scene, GBufferTargetDescs* gbuffer) noexcept;

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

    // w = 1 because this is a point: a matrix that multiplies it must reach it with
    // its translation. Nothing multiplies it today, so the value is a contract rather
    // than an effect.
    glm::vec4 viewPos;      // xyz = camera position, w = 1
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
// One light, in the form a shader reads
//
// **A projection of LightState, and the kind is what it loses.** What shading needs is
// not the name of a category but a direction, a position and a cone, and the three
// kinds differ only in which of those mean anything:
//
//   directional   w = 0, so the direction is the same everywhere and nothing falls off
//   spot          w = 1, position and cone both read
//   point         w = 1, cone opened all the way -- a point light is a spot with no cone
//
// The last line is why there is no kind field here. LightState declares which it is and
// this is what that declaration comes to; opening the cone is not a guess about the
// kind, it is what a point light's cone is.
struct LightEntry {
    // xyz = surface toward the light, w = 0 for a direction and 1 for a position, which
    // is what w means in homogeneous coordinates. Not a flag beside the vector: the
    // shader reads whether translation reaches it out of the same number.
    glm::vec4 direction;

    glm::vec4 color;       // rgb = colour, a unused
    glm::vec4 position;    // xyz = where it is, w = how far its light carries
    glm::vec4 cone;        // x = inner cosine, y = outer, zw unused
};

struct LightUniform {
    LightEntry lights[kMaxLights];

    // rgb = the ambient the sky adds under all of them, a = how many entries above are
    // live. A count and not a sentinel: a loop that stops on one has to read a light to
    // find out it should not have.
    glm::vec4 ambient;
};

// Output: what one light comes to, in the form a shader reads
//
// The projection LightEntry's comment describes, in one place: a directional light
// writes w = 0 and nothing else matters; a spot writes its cone; a point opens the cone
// all the way, which is what a point light's cone is rather than a way of marking it.
LightEntry EntryFor(const LightState& light) noexcept;

// Effect: fills a LightUniform from up to kMaxLights states
//
// Refuses more than fit rather than dropping the last quietly -- a light that is not
// there is a picture nobody can explain. The ambient is an argument because it is the
// scene's and not any one light's.
bool FillLights(const LightState lights[], uint32_t count, const glm::vec3& ambient,
                LightUniform* out) noexcept;

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
// The same switches as the shader reads them.
//
// Here rather than in SceneUniform, which is where they used to sit. What they answer
// to is the panel, not the scene: nothing about a camera or a light decides them, and
// the pass whose uniform carried them had no say in any of it. Moving them out left
// SceneUniform with only values the frame actually computes.
//
// Floats rather than a bitfield: std140 packs them into whole vec4s either way, and
// this way each has a name on both sides of the boundary instead of a bit position.
// 0 or 1, and the shader compares against 0.5 so a half value is not a third state.
//
// Contract: field order matches the View block in scene.frag.
struct ViewOptionsUniform {
    float useNormalMap;
    float useBaseColor;
    float useSpecular;
    float useAlphaMask;
    float useShadow;
    float useMetallicRoughness;

    // 0 shows the lit result, 1..4 show one of the geometry pass's images instead.
    // Read by lighting.frag alone -- scene.frag declares the six above and stops,
    // which is what a stage taking the front of a block is allowed to do.
    //
    // Here rather than on the CPU side of the panel because the value has to reach a
    // shader: nothing about which image to show can be decided by a command.
    float channel;

    float pad;   // std140 rounds the block up to a second vec4
};



// The panel's answers as one per frame in flight, the way the camera and the light
// are. The fourth of exactly the same kind, and the last to get here: it lived inside
// Gui until 09-06, which meant three passes had to know what a Gui was to name it.
//
// Who decides and who sends are different questions. The panel still decides -- the
// checkboxes are its, GuiViewUniform builds this out of them -- and UploadFrameValues
// sends it, which is what happens to the other three as well.
struct FrameViewOptions {
    ViewOptionsUniform value{};
    Buffer buffer;
};

bool CreateFrameCameras(const VulkanDevice& dev, FrameCamera* out) noexcept;
bool CreateFrameLights(const VulkanDevice& dev, FrameLight* out) noexcept;
bool CreateFrameShadows(const VulkanDevice& dev, FrameShadow* out) noexcept;
bool CreateFrameViewOptions(const VulkanDevice& dev, FrameViewOptions* out) noexcept;

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


// Every uniform block this renderer owns, by the name a shader gives it
// ============================================================================
//
// **Renderer owns the contract. Shader implements it. Reflection validates it.** These
// five structs are the contract; the shaders declare the part each stage reads; the
// offsets below come from offsetof and sizeof, so moving a field moves the requirement
// with it and a shader that did not move is refused at startup.
//
// This is what the "field order and std140 padding match X" comments in the shaders
// used to say, and until 09-06 nothing checked them.
//
// **Keyed by name and not by a slot**, because the same block sits at different
// bindings in different programs -- shadow is binding 0 in shadow.vert and binding 2
// in scene.frag. A requirement written against a slot would reach one of them.
//
// Padding is left out. No shader declares it, and a member no stage reads is compared
// by nobody.
//
// Contract: the names are the instance names in GLSL -- camera, light, shadow, view,
//           mtl -- not the block type names.
inline ProgramRequirements SharedBlocks() noexcept {
    static const RequiredMember kCamera[] = {
        {"view",     offsetof(CameraUniform, view),     sizeof(CameraUniform::view)},
        {"proj",     offsetof(CameraUniform, proj),     sizeof(CameraUniform::proj)},
        {"viewPos",  offsetof(CameraUniform, viewPos),  sizeof(CameraUniform::viewPos)},
    };
    static const RequiredMember kLight[] = {
        {"lights",  offsetof(LightUniform, lights),  sizeof(LightUniform::lights)},
        {"ambient", offsetof(LightUniform, ambient), sizeof(LightUniform::ambient)},
    };
    static const RequiredMember kShadow[] = {
        {"lightView", offsetof(ShadowUniform, lightView), sizeof(ShadowUniform::lightView)},
        {"lightProj", offsetof(ShadowUniform, lightProj), sizeof(ShadowUniform::lightProj)},
    };
    static const RequiredMember kView[] = {
        {"useNormalMap",  offsetof(ViewOptionsUniform, useNormalMap),
                          sizeof(ViewOptionsUniform::useNormalMap)},
        {"useBaseColor",  offsetof(ViewOptionsUniform, useBaseColor),
                          sizeof(ViewOptionsUniform::useBaseColor)},
        {"useSpecular",   offsetof(ViewOptionsUniform, useSpecular),
                          sizeof(ViewOptionsUniform::useSpecular)},
        {"useAlphaMask",  offsetof(ViewOptionsUniform, useAlphaMask),
                          sizeof(ViewOptionsUniform::useAlphaMask)},
        {"useShadow",     offsetof(ViewOptionsUniform, useShadow),
                          sizeof(ViewOptionsUniform::useShadow)},
        {"useMetallicRoughness", offsetof(ViewOptionsUniform, useMetallicRoughness),
                          sizeof(ViewOptionsUniform::useMetallicRoughness)},
        {"channel",       offsetof(ViewOptionsUniform, channel),
                          sizeof(ViewOptionsUniform::channel)},
    };
    static const RequiredMember kMaterial[] = {
        {"baseColorFactor", offsetof(MaterialParams, baseColorFactor),
                            sizeof(MaterialParams::baseColorFactor)},
        {"alphaCutoff",     offsetof(MaterialParams, alphaCutoff),
                            sizeof(MaterialParams::alphaCutoff)},
        {"metallic",        offsetof(MaterialParams, metallic),
                            sizeof(MaterialParams::metallic)},
        {"roughness",       offsetof(MaterialParams, roughness),
                            sizeof(MaterialParams::roughness)},
    };

    static const RequiredBlock kBlocks[] = {
        {"camera", kCamera,   static_cast<uint32_t>(std::size(kCamera))},
        {"light",  kLight,    static_cast<uint32_t>(std::size(kLight))},
        {"shadow", kShadow,   static_cast<uint32_t>(std::size(kShadow))},
        {"view",   kView,     static_cast<uint32_t>(std::size(kView))},
        {"mtl",    kMaterial, static_cast<uint32_t>(std::size(kMaterial))},
    };

    // The push block. No name -- a stage has one or none -- and every stage that
    // declares one is held to it, whichever part of it that stage reads: shadow.vert
    // takes model alone, scene.vert takes model and normal, and the fragment stages
    // take alpha at the offset the two in front of it leave.
    static const RequiredMember kPush[] = {
        {"model",  offsetof(PushConstants, model),  sizeof(PushConstants::model)},
        {"normal", offsetof(PushConstants, normal), sizeof(PushConstants::normal)},
        {"alpha",  offsetof(PushConstants, alpha),  sizeof(PushConstants::alpha)},
    };

    ProgramRequirements out;
    out.blocks = kBlocks;
    out.blockCount = static_cast<uint32_t>(std::size(kBlocks));
    out.pushMembers = kPush;
    out.pushMemberCount = static_cast<uint32_t>(std::size(kPush));
    return out;
}


// What one draw is
// ============================================================================

// No material. A DrawItem's index defaults to this, so an item nobody assigned one to
// is not silently the material that happens to sit at 0.
//
// UINT32_MAX and not -1: it is also above every valid index, so one comparison against
// the array's size catches both an unset item and an index past the end.
constexpr uint32_t kNoMaterial = UINT32_MAX;

// Output: the cull mode every draw should use, or this sentinel to leave it to
//         each material. Outside VkCullModeFlagBits, so no real value collides.
//
// Beside kNoMaterial because it is the same kind of value: a number outside the
// range of real ones, standing for "nobody chose". The panel produces it and two
// passes read it, so it belongs to neither of them.
constexpr VkCullModeFlags kCullFromMaterial = UINT32_MAX;

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
    // Set together through SetDrawTransform: normal is derived from model and the two
    // must not be written apart.
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

// Effect: derives a draw's two matrices from the transform they represent
//
// Takes state and not a matrix, which is the same cut the camera and the light were
// given: what an object is belongs to whatever owns the scene, and both matrices are
// this layer's rendering of it.
//
// One call because the two are one fact. Writing model alone leaves the normals
// answering to the previous transform -- invisible while every scale is uniform, and
// silently wrong after that.
void SetDrawTransform(DrawItem* item, const Transform& transform) noexcept;


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


// What recording one pass of draws cost in state changes.
//
// Counted where it happens rather than worked out from the item list, so the number is
// what the command buffer actually got. These are what a sort order changes: the draws
// are fixed, the other two are not.
//
// They do not fall together. materialBinds reaches its floor -- the number of distinct
// materials -- as soon as equal materials are adjacent. cullChanges reaches its floor
// only if the order groups by cull first, which sorting on the material alone does not
// do even though cull is a function of it.
// One resource another pass produced, in the two forms a reader needs it
//
// **resource is the identity and frames[] is this frame's copy of it.** Every Texture
// keeps its own copy of the desc it was made from, so &frames[0]->desc and
// &frames[1]->desc are different addresses holding equal values -- an identity taken
// from one of them would be a different identity every frame in flight. resource points
// at the one main declared and made all of them from, which outlives a resize while the
// images do not.
//
// A type of its own so that a read and a write cannot be swapped. Both were
// const Texture* const[kFramesInFlight] until now, and the only thing telling
// CreateLightingPass's source from its target was the parameter name.
//
// Contract: every frames[i] describes the same thing as *resource. DeclareRead checks
//           it rather than assuming, because nothing else would notice.
struct PassInput {
    const TextureDesc* resource = nullptr;
    const Texture* frames[kFramesInFlight]{};
};

// Everything set 0 holds, declared once instead of spelled out per pass
//
// **The whole of it goes to every pass that uses the frame set, and each program takes
// the subset it declared.** That is the same relation a VertexLayout has with a vertex
// stage: the CPU side says what the resource holds and the shader reads part of it.
// Reflection turns the unread bindings into holes and UpdateSet skips them, so the
// three empty entries the geometry pass used to write by hand were mirroring a layout
// that already knew.
//
// Written in three files before this, positionally -- binding 0 is the camera, 1 the
// light, and so on -- with nothing but comments keeping the three in step.
//
// shadowMap is the one member of a different kind. The other four are the renderer's
// own buffers, filled by a memcpy each frame; this one is another pass's product, and
// so the only member that makes an edge.
struct FrameSetSources {
    const FrameCamera* cameras = nullptr;         // binding 0
    const FrameLight* lights = nullptr;           // binding 1
    const FrameShadow* shadows = nullptr;         // binding 2
    PassInput shadowMap;                          // binding 3
    const FrameViewOptions* views = nullptr;      // binding 4

    // The environment. One texture and not a PassInput, and the reason is not that no
    // pass makes it -- one does, six faces of it at startup -- but that it is not made
    // again. What decides whether a resource is an edge in this graph is how often it
    // is remade against the span the graph covers, which is one frame. A sky rebaked
    // for a time of day would be the same image and would be an edge.
    //
    // So it sits with the material textures and the mesh: an input the frame is handed,
    // not something the frame produces.
    const Texture* skyCube = nullptr;              // binding 5

    // What a matte surface receives from the whole sky, convolved once from the cube
    // above. Same category as it -- made once, never again.
    const Texture* irradianceCube = nullptr;       // binding 6

    // The other half of the environment: what a shiny surface reflects, blurred into
    // the mip chain by roughness, and the table that says what the BRDF does with it.
    const Texture* prefilteredCube = nullptr;      // binding 7
    const Texture* brdfLut = nullptr;              // binding 8
};

// Effect: writes this frame's set 0, and declares in desc the pass outputs this
//         program actually reads
//
// **What is read is the program's answer, not this function's.** A binding the shader
// never mentions is a hole in the layout, so the geometry pass gets no shadow map and
// no edge for one -- and neither statement is written down twice.
bool FillFrameSet(const Descriptors& descriptors, const ShaderProgram& program,
                  VkDescriptorSet set, const FrameSetSources& sources, uint32_t frame,
                  RenderPassDesc* desc) noexcept;

// Effect: checks every frame's image is the kind this pass reads, then records the
//         resource in desc->reads[] once
//
// what and wantDepth are the pass's knowledge rather than its caller's, so they are
// arguments here. This is where CheckSampledInput's job moved: the check is the same
// and the identity it used to discard is kept.
bool DeclareRead(const PassInput& input, const char* what, bool wantDepth,
                 RenderPassDesc* desc) noexcept;

struct DrawStats {
    uint32_t draws = 0;
    uint32_t materialBinds = 0;
    uint32_t cullChanges = 0;
};

// What the panel decided about rasterization, as the six values a pass of draws
// uses. Read by the scene pass and by the geometry pass, which is why it is here.
// Gathered into one struct so the signature does not grow a parameter per checkbox.
//
// Two kinds in one struct, and the first field is the odd one:
//
//   polygonMode   picks which pipeline is bound. It is compiled in, so a second
//                 value is a second pipeline
//   the five      go out as vkCmdSet* after that pipeline is bound. This pass is
//                 where their values are decided, and the panel is where they live
//
// The five are the whole of this pass's dynamic state apart from viewportY, which no
// checkbox reaches -- so the pipeline desc names viewportY and nothing else.
struct RasterOptions {
    // A polygonMode and not a bool, unlike the five below it. Those are VkBool32 or a
    // flag on the other side; this one is a three-valued enum, and calling it
    // "wireframe" meant only two of the three could be asked for.
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    bool depthTest = true;
    bool depthWrite = true;
    bool rasterizerDiscard = false;
    VkCullModeFlags cull = kCullFromMaterial;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
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
                       const FrameShadow* shadows, FrameViewOptions* views,
                       const Gui& gui) noexcept;

// Effect: resets the slot's command buffer and records this frame's passes into it
// Output: false means the buffer is invalid and must not be submitted
//         stats, if given, is what the pass that walked the draw list cost. Every
//         frame records the same list, so one frame's numbers are the answer.
//
// **Which passes there are is the panel's answer, read here and nowhere else.** Every
// pass below is created either way and holds its own sets; what the switch changes is
// which of them this function names. That is what makes it a switch rather than a
// rebuild -- and it is also the whole of what forward and deferred differ by, once the
// shaders are written: four lines in one function.
//
// The shadow pass and everything after the middle run in both. A frame is
//
//   forward    shadow  scene                post  gui
//   deferred   shadow  geometry  lighting   post  gui
//
// and the middle writes the same image either way.
//
// Takes the slot but never touches its fence or semaphore -- a rule, not a type.
// A Texture, not the whole FrameTarget: nothing here reads the index or the semaphore,
// and those belong to getting the frame out, not to drawing it.
//
// Contract: UploadFrameValues has run for this slot. What is recorded here reads
//           those buffers, and nothing in the command stream would say they are stale.
bool RecordFrame(const FrameSlot& slot,
                 const ShadowPass& shadow,
                 const SkyPass& skyForward, const SkyPass& skyDeferred,
                 const ScenePass& scene,
                 const GeometryPass& geometry, const LightingPass& lighting,
                 const PostProcessPass& post, Gui& gui, const Texture& target,
                 const DrawList& draws, DrawStats* stats = nullptr) noexcept;
