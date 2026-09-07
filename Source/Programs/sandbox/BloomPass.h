#pragma once

// BloomPass - what is brighter than the display, spread out and added back
// ============================================================================
//
// Three draws over two half-size images. Extract reads the middle's output and keeps
// what is above the display's range; the blur runs twice, once along each axis, because
// a 2D Gaussian is the product of two 1D ones and two passes of nine taps cost eighteen
// samples where one pass of the same width costs eighty-one.
//
//   extract   scene -> A
//   blur      A -> B, horizontally
//   blur      B -> A, vertically
//
// A and B are two images and not one because a pass cannot read the image it writes.
// The post pass reads A, which is why the vertical run is the one that lands there.
//
// **It only means anything because the scene chain is a float.** On an 8-bit target the
// sun the sky is baked with -- two hundred times a lit wall -- arrives as plain white,
// and a threshold would be cutting along a line the renderer drew rather than one the
// lighting did.
//
// Half size, and that is the blur as much as the cost: a bilinear tap at half resolution
// already averages four pixels, so the nine taps reach twice as far as their count
// suggests.

#include "Passes.h"

// Output: what one of the two bloom images is, from the size of what it blurs
//
// Half of the scene's, rounded up so a 1281-wide render still has somewhere to put its
// last column. The same float format the scene chain uses: what is stored here is still
// radiance, and it is added back before anything tone-maps.
TextureDesc MakeBloomTarget(VkExtent2D sceneExtent) noexcept;

// One texel along the axis being blurred, as the push constant the blur takes.
//
// Contract: matches Push in bloom_blur.frag, and blurStep sits where a draw's model
//           matrix does in the programs that draw geometry. The two never appear in one
//           program, which is the arrangement the shadow passes' light index uses.
struct BloomStep {
    float x = 0.0f;
    float y = 0.0f;
};

struct BloomPass {
    // Both images have the same shape, and they are still two declarations: an edge in
    // the frame graph is two passes pointing at one desc, so one desc for both would
    // make "reads what the last pass wrote" true by construction rather than by fact.
    TextureDesc aDesc;
    TextureDesc bDesc;

    RenderPassDesc extract;
    RenderPassDesc blurH;
    RenderPassDesc blurV;

    const Pipeline* extractPipeline = nullptr;
    const Pipeline* blurPipeline = nullptr;

    struct PerFrame {
        // **Borrowed.** main makes them, because a resize remakes them and the post
        // pass names one of them in its own set.
        const Texture* a = nullptr;
        const Texture* b = nullptr;

        // What the extract step samples, kept for the same reason the other two are:
        // a resize replaces the view inside it and the set has to be written again.
        const Texture* source = nullptr;

        // One per draw, each naming what that draw samples.
        VkDescriptorSet extractSet = VK_NULL_HANDLE;
        VkDescriptorSet blurHSet = VK_NULL_HANDLE;
        VkDescriptorSet blurVSet = VK_NULL_HANDLE;
    };
    PerFrame frames[kFramesInFlight];
};

// Effect: rewrites the three sets to name this frame's images again
//
// A resize replaces the views inside the textures, and a set records a handle. Same
// event and same reason as RefreshPostProcessPass.
void RefreshBloomPass(const Descriptors& descriptors, BloomPass* bloom) noexcept;

// Contract: sceneDesc, aDesc and bDesc outlive this pass -- the declarations point at
//           them, and the post pass points at the same aDesc.
// Contract: source, a and b each hold kFramesInFlight entries, frame for frame.
bool CreateBloomPass(const Descriptors& descriptors,
                     const PassInput& source,
                     const TextureDesc& aDesc, const Texture* const a[kFramesInFlight],
                     const TextureDesc& bDesc, const Texture* const b[kFramesInFlight],
                     const Pipeline& extractPipeline, const Pipeline& blurPipeline,
                     BloomPass* out) noexcept;

// Effect: appends the three draws and leaves image A readable by the post pass
void RecordBloomPass(const FrameSlot& slot, const BloomPass& bloom) noexcept;
