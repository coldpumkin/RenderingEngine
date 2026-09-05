#pragma once

// PostProcessPass - reads what the scene pass produced, writes the frame's target
// ============================================================================
//
//   what            where it comes from      who else uses it
//   the source      main makes it            the scene pass draws into it
//   the target      the presentation engine  the gui pass draws on top
//   the set         this pass makes it       nobody
//
// Owns nothing. It takes the images, not the passes that made them: everything it
// needs of its input is what a Texture already says.

#include "Passes.h"

// **It takes the images, not the pass that made them.** Everything this pass needs of
// its input is what a Texture already says -- extent, format, one sample -- and none
// of those three is the scene's to decide. A multisample image cannot be sampled, so
// the resolve exists for this reader; the format has to mean what post.frag
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
// No attachments of its own: the image it draws into arrives with the frame, one of
// however many the swapchain handed back rather than one per frame in flight. What it
// can hold is the description of that image, which is the same for all of them.
struct PostProcessPass {
    // One per frame in flight. Non-owning: the scene pass owns these images.
    const Texture* source[kFramesInFlight]{};

    // What it writes, described the way the other passes' targets are. Non-owning, and
    // **the format is the part that keeps**: the extent belongs to whichever image
    // arrives, which is why RecordFrame reads it off that image and not off here.
    const TextureDesc* target = nullptr;

    // No program: it is the pipeline's, which records what it was built from.
    const Pipeline* pipeline = nullptr;   // the one variant. non-owning

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
// Output: false also means what it reads or what it writes disagrees with the
//         pipeline -- the two checks the other passes make, which this one could not
//         until it was told what it writes
//
// Contract: source[i] is created and outlives this pass. That it is 1-sample is
//           checked here now: a multisample image cannot be bound to a sampler.
bool CreatePostProcessPass(const Descriptors& descriptors,
                           const Texture* const source[kFramesInFlight],
                           const TextureDesc& target,
                           const Pipeline& pipeline, PostProcessPass* out) noexcept;

// Output: what the post pipeline is compiled from
//
// No vertex layout: fullscreen.vert builds its three points from gl_VertexIndex, and
// a shader that makes its own vertices needs no buffer described. No depth either --
// nothing here is hidden behind anything.
//
// Contract: target must outlive CreateGraphicsPipeline. The desc points at it.
GraphicsPipelineDesc MakePostPipeline(const ShaderProgram& program,
                                      const TextureDesc& target) noexcept;


// Input:  the pass (its source and pipeline), the slot (cmd, which frame), and the
//         texture to draw into
// Effect: appends commands that sample the scene pass's resolve into that texture
//
// A Texture, not the whole FrameTarget: nothing here reads the index or the semaphore,
// and those belong to getting the frame out, not to drawing it. Drawing somewhere else
// -- the next stage of an effect chain, a screenshot -- is then a different argument,
// not a different function.
//
// The target is left COLOR_ATTACHMENT_OPTIMAL. What happens to it next is the frame's
// to decide, not this pass's.
void RecordPostProcessPass(const FrameSlot& slot, const PostProcessPass& post,
                           const Texture& target) noexcept;

