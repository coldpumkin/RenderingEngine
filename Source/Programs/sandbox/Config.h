#pragma once

#include <cstdint>

// Config - compile-time knobs. kDesired* are requests the driver may lower, the rest
// are ours outright. No Vulkan header, so main assembles the extent.
//
// Scene values are not here: the sphere's stacks, the light's colour and the like are
// test data that a loader replaces, and Config would then describe a scene it has no
// business knowing.

// 1 and 2 measured identical at 120Hz -- 90% of the frame is the acquire wait.
//
// **At 1 this whole axis is one element wide.** Seven things are sized by this number
// -- the slot, the light, each pass's per-frame share, the gui's buffers -- and they
// are paired by slot.index and by nothing else, so an index used wrongly cannot be
// told from an index used rightly while there is only one. That is a measurement gap
// and not a bug, and it was closed by measuring rather than by reasoning: built at 2,
// the captured frame is byte-identical, sync validation says nothing, and the resize
// and minimise cycle survives. Worth repeating whenever something new joins the axis.
constexpr uint32_t kFramesInFlight = 1;

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

// Half-width of that box, in world units, and how far back the light sits. Sponza is
// about 20 x 12 x 12 after its scale, so this covers it with room for the light to
// swing around. Too large and every texel spans more world than it can resolve.
constexpr float kShadowRadius = 13.0f;
constexpr float kShadowDistance = 22.0f;

// ChooseRenderTargetFormats lowers it to what color and depth both support. No 1x
// path: there the resolve attachment is illegal, and it would never run here anyway.
constexpr uint32_t kDesiredSampleCount = 4;

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
