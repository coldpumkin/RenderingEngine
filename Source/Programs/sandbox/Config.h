#pragma once

#include <cstdint>

// Config - compile-time knobs. kDesired* are requests the driver may lower, the rest
// are ours outright. No Vulkan header, so main assembles the extent.
//
// Scene values are not here: the sphere's stacks, the light's colour and the like are
// test data that a loader replaces, and Config would then describe a scene it has no
// business knowing.

// 1 and 2 measured identical at 120Hz -- 90% of the frame is the acquire wait.
constexpr uint32_t kFramesInFlight = 1;

// A different axis: frames-in-flight is how far the CPU runs ahead, this is how many
// images rotate. 3 because a busy GPU misses vsync with 2 (Vulkan-Samples).
constexpr uint32_t kDesiredSwapchainImages = 3;

// Independent of the window. Fixed: following it adds a rebuild path for the targets.
constexpr uint32_t kRenderWidth = 1280;
constexpr uint32_t kRenderHeight = 720;

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
