#pragma once

#include <cstdint>

#include <volk.h>   // VkFormat, for the render chain's colour below

// Config - compile-time knobs. kDesired* are requests the driver may lower, the rest
// are ours outright. No Vulkan header, so main assembles the extent.
//
// Scene values are not here: the sphere's stacks, the light's colour and the like are
// test data that a loader replaces, and Config would then describe a scene it has no
// business knowing.

// 1 and 2 measured identical at 120Hz -- 90% of the frame is the acquire wait.
//
// **2 since 09-06, and the reason is that 1 cannot be checked.** Eighty-nine places
// are sized by this number -- the slot, the camera, the light, the panel's switches,
// each pass's per-frame share, the g-buffer, the gui's vertices -- and every one of
// them is paired with its frame by slot.index and by nothing else. At 1 an index used
// wrongly reads the same memory as an index used rightly, so the whole axis is a
// claim no run can refute. At 2 it is checked on every frame.
//
// That is the same rule that keeps a 1x MSAA fallback out of this program: code that
// does not run here cannot be verified.
//
// Measured at 2 on both paths, 09-06: build warnings 0, validation 0, synchronization
// validation 0, four resizes plus minimize and restore, and **both capture hashes
// byte-identical to the values taken at 1**. Slots holding different resources and
// the picture not moving is what says the pairing is right.
//
// The cost is memory and it is the only cost. Per frame in flight, at 1280x720:
// SceneTargets 31.6 MB (4x colour + resolve + 4x depth), GBufferTargets 14.1 MB, the
// shadow map 16.0 MB -- 61.7 MB, doubled. Speed is unchanged, see the first line.
//
// **Rerun whenever something new joins the axis.** Three things did on 09-06.
constexpr uint32_t kFramesInFlight = 2;

// A different axis: frames-in-flight is how far the CPU runs ahead, this is how many
// images rotate. 3 because a busy GPU misses vsync with 2 (Vulkan-Samples).
constexpr uint32_t kDesiredSwapchainImages = 3;

// Independent of the window, and how independent is the line below.
constexpr uint32_t kRenderWidth = 1280;
constexpr uint32_t kRenderHeight = 720;

// Whether the render targets take the window's size instead.
//
//   false  the targets stay kRenderWidth x kRenderHeight and LetterboxInto fits the
//          picture into whatever the window is
//   true   the targets are remade on every resize and the letterbox is an identity
//
// A panel switch until 09-05, which bought exactly one thing: both paths reachable.
// A constant buys that too, by rebuilding -- the way kFramesInFlight above is
// verified -- and it stops a convenience feature from deciding how big a render
// target is. Nothing but the renderer has a claim on that.
constexpr bool kRenderFollowsWindow = false;

// The shadow map, square because the light's ortho box is. Independent of the render
// resolution: what decides it is how much world one texel covers, not how many pixels
// end up looking at it.
constexpr uint32_t kShadowResolution = 2048;

// A face of a point light's cube. Smaller than the directional map because six of them
// cover what one 2D map covers, and because a point light here lights a room rather than
// the whole scene.
constexpr uint32_t kPointShadowResolution = 512;

// Half-width of that box, in world units, and how far back the light sits. Sponza is
// about 20 x 12 x 12 after its scale, so this covers it with room for the light to
// swing around. Too large and every texel spans more world than it can resolve.
constexpr float kShadowRadius = 13.0f;
constexpr float kShadowDistance = 22.0f;

// ChooseRenderTargetFormats lowers it to what color and depth both support. No 1x
// path: there the resolve attachment is illegal, and it would never run here anyway.
constexpr uint32_t kDesiredSampleCount = 4;

// What every target in the render chain is made of, and not the swapchain's: the two
// hold the same value today and part the day post tone-maps, which wants a float.
// R8G8B8A8 because WriteBmp reads red first; SRGB so blending and the resolve run in
// linear space.
constexpr VkFormat kRenderColorFormat = VK_FORMAT_R8G8B8A8_SRGB;

// The window opens at this size and the user resizes from there. Unlike kRender*,
// which never follows the window.
constexpr uint32_t kWindowWidth = 1280;
constexpr uint32_t kWindowHeight = 720;

// Vertical field of view. The near plane trades depth precision for how close you can
// get; 0.1 is far enough from 0 to keep the depth buffer usable.
constexpr float kFovDegrees = 60.0f;
constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 100.0f;

// Per second, so frame rate does not change how fast the camera moves.
constexpr float kMoveSpeed = 2.0f;    // world units
constexpr float kTurnSpeed = 90.0f;   // degrees
