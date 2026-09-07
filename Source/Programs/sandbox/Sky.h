#pragma once

// Sky - a cube map, and the pass that shows it
// ============================================================================
//
//   what                     when                        who reads it
//   ----------------------------------------------------------------------
//   the cube's contents      once, at startup            every frame after
//   which face is drawn      six times inside that       one render pass each
//   the direction sampled    per pixel, every frame      sky.frag
//
// **The cube is an input to the frame and not an edge in it**, and the reason is the
// middle column rather than anything about what it holds: it is made once and never
// again, which puts it with the material textures and the mesh. A sky that was rebaked
// for a time of day would be an edge, as the same image, so what decides is how often
// it is remade against the span the graph covers.
//
// A cube map is a texture addressed by a direction; six faces are how that is stored.
// What is in it is a separate question -- radiance here, and a distance from a point if
// this is ever an omnidirectional shadow.

#include "Passes.h"
#include "Vulkan/Commands.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Texture.h"

// Output: what the sky cube is
//
// 16-bit float because a sky is the brightest thing in a scene and an 8-bit one clips
// where an IBL would want the range. 256 a side is enough for a gradient and is what a
// prefilter chain would start from.
TextureDesc MakeSkyTarget() noexcept;

// Contract: matches Face in skybake.frag. Which of the six is being drawn, and nothing
//           else -- a face of a cube is the same whichever way anyone is looking.
struct SkyFace {
    int32_t index = 0;
};

// Output: what the diffuse irradiance cube is
//
// Small on purpose. What it holds is an integral over a hemisphere, so it has no detail
// to lose -- 32 a side is the usual size and more would be storing the same numbers
// again. Same format as the sky, because it is the same kind of quantity.
TextureDesc MakeIrradianceTarget() noexcept;

// Effect: convolves environment into every face of irradiance, then leaves it readable
//
// The same six-pass shape the sky bake has, with one difference that costs a descriptor
// set: this program reads a cube while it writes one. Its own set, not the frame set --
// nothing about a frame is in this, and it runs before any frame exists.
bool BakeIrradianceCube(const VulkanDevice& dev, const Commands& commands,
                        const Descriptors& descriptors, const Pipeline& pipeline,
                        const Texture& environment, Texture* irradiance) noexcept;

// Effect: draws the sky into all six faces and leaves the cube readable by a sampler
//
// Six render passes, one per layer, each through a 2D view of that layer. One pass over
// all six would need either multiview or a stage that can write gl_Layer, and neither is
// a feature this asks a device for.
//
// The views are made and destroyed here: nothing after this addresses a single face, so
// keeping them would be keeping six handles for one moment.
bool BakeSkyCube(const VulkanDevice& dev, const Commands& commands,
                 const Pipeline& pipeline, Texture* cube) noexcept;

// The pass that draws it, once per path
//
// Two of these exist, because the two paths fill different images: the forward one
// draws into the multisample scene colour and the deferred one into the single-sample
// image the lighting pass writes. Same program, one pipeline each, and the sample count
// is the whole of the difference.
struct SkyPass {
    const Pipeline* pipeline = nullptr;
    RenderPassDesc pass;

    struct PerFrame {
        VkDescriptorSet set = VK_NULL_HANDLE;
        const Texture* target = nullptr;
    };
    PerFrame frames[kFramesInFlight];
};

// Contract: targets[i] is what this path's middle draws into, and every one of them has
//           the shape targetDesc describes.
bool CreateSkyPass(const Descriptors& descriptors,
                   const TextureDesc& targetDesc,
                   const Texture* const targets[kFramesInFlight],
                   const Pipeline& pipeline,
                   const FrameSetSources& frameSet,
                   SkyPass* out) noexcept;

// Effect: fills the target with the sky, and hands it to the pass that loads it next
//
// No depth attachment and no depth test: every pixel is written and whatever draws
// afterwards covers what it covers. Testing depth instead would need the middle's depth
// written and stored first, which is a different arrangement of the frame.
void RecordSkyPass(const FrameSlot& slot, const SkyPass& sky) noexcept;
