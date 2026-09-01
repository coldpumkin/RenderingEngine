#pragma once

#include <cstdint>

// Config - compile-time knobs. kDesired* are requests the driver may lower, the rest
// are what we get. No Vulkan header, so main assembles the extent.

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
