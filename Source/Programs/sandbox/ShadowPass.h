#pragma once

// ShadowPass - the same surfaces, depth only, from where the light is
// ============================================================================
//
//   what           where it comes from     who else uses it
//   the map        main makes it           the scene pass samples it
//   the matrix     FrameShadow, handed in  scene.frag reads the same buffer
//   mesh, draws    the scene pass's        the scene pass draws the same list
//   the set        this pass makes it      nobody
//
// Nothing in the first three rows is owned here. What the pass is, is a pipeline it
// names and one descriptor set per frame in flight.

#include "Passes.h"

// Output: the one image the shadow pass makes
//
// Input is the shape every target maker here takes: how big, what this caller chose,
// and what the device answered. This one chooses nothing, so the middle is absent.
//
// **caps.samples is read and refused.** One sample, always: averaging depths across an
// edge produces a value no surface was ever at, and every fragment comparing against
// it is wrong. Taking the whole of what the device offers is what makes that a
// decision here rather than an argument main forgot to pass.
//
// SAMPLED because the scene pass reads it -- the second of the two edges.
TextureDesc MakeShadowTarget(VkExtent2D extent, const TargetCapabilities& caps) noexcept;

// The first pass here with no colour attachment. Its product is a depth image the
// scene pass samples, which makes it also the first thing depth does outside the
// frame that produced it.
//
// Its own set, but not its own viewpoint: the matrix it draws with is a FrameShadow,
// handed in, and scene.frag reads that same buffer.
struct ShadowPass {
    const Mesh* mesh = nullptr;

    // No program: a pipeline records the one it was built from, and what a draw
    // receives is the pipeline's fact rather than the pass's.
    const Pipeline* pipeline = nullptr;

    // Per frame in flight for the reason the scene's attachments are: the GPU still
    // reads the previous frame's map while the next is drawn.
    struct PerFrame {
        // **Borrowed.** main makes it and hands the same array to this pass and to the
        // scene pass, so the one image has one name that both can say.
        //
        // Its desc asks for DEPTH_STENCIL_ATTACHMENT to draw into and SAMPLED to be
        // read afterwards, at one sample -- multisampling a visibility test would
        // average depths no surface was ever at. Checked below against the pipeline.
        const Texture* depth = nullptr;

        // Names the FrameShadow of the same index.
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    PerFrame frames[kFramesInFlight];
};

// Effect: takes the maps it draws into and makes the set naming its matrix
//
// It made those maps until 09-05, from a TextureDesc handed in. main owns them now,
// which is what lets the scene pass be handed the same array instead of walking into
// frames[i] to find them -- and is why there is no VulkanDevice argument left: this
// creates nothing but descriptor sets, and the pool knows its device.
//
// Contract: maps and shadows each hold kFramesInFlight entries and outlive this pass.
//           Each set names the buffer of the same index; each map is drawn into by
//           the frame of the same index.
bool CreateShadowPass(const Descriptors& descriptors,
                      const Texture* const maps[kFramesInFlight],
                      const Mesh& mesh,
                      const Pipeline& pipeline, const FrameShadow* shadows,
                      ShadowPass* out) noexcept;

// Input:  the pass, the slot that carries the command buffer, and this frame's list
// Effect: appends the commands that draw this slot's map
//
// No swapchain, so this works without a window. No camera either: the matrix it draws
// with is in the set, and every draw here reads it.
//
// Depth is the whole product, so nothing binds a material or sets a cull mode.
void RecordShadowPass(const FrameSlot& slot, const ShadowPass& shadow,
                      const DrawList& draws) noexcept;

