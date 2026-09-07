#pragma once

// PointShadowPass - the same surfaces again, six times, from where a point light is
// ============================================================================
//
//   what              where it comes from      who else uses it
//   the cube array    main makes it            the two shading passes sample it
//   the face views    this pass makes them     nobody
//   the matrices      FramePointShadow         the same buffer the fragment stage reads
//   mesh, draws       the scene's              every pass draws the same list
//
// A 2D map is one direction. A point light has every direction, so its map is a cube and
// filling one is six render passes rather than one. That is the whole of what makes this
// pass different from ShadowPass; the loop body is the same.
//
// What is stored is the distance from the light, divided by its range, rather than a
// depth. A depth would belong to one face's projection, and reading it back would mean
// working out which face a direction landed on and undoing that projection. A distance
// is the same number whichever face it came from.

#include "Passes.h"

// Output: the cube array every point light's shadow is drawn into
//
// One cube per light slot, so light i owns layers 6i .. 6i+5 and nothing translates
// between the two indices -- the same rule the 2D array follows.
//
// R16_SFLOAT because what is stored is a distance already divided by the range, so the
// values are 0..1 and half a float carries them with room to spare. Colour rather than
// depth: it is a number this renderer computes, not one the depth test produces.
TextureDesc MakePointShadowTarget() noexcept;

// Output: the depth the faces are drawn with
//
// One layer per face rather than one image reused, and that is not about memory. Six
// passes writing one depth image in a row are six writes with nothing ordering them, and
// the layer is what makes each pass's target its own.
//
// Not sampled by anything. It exists so the nearest surface wins, and it is thrown away
// with the pass.
TextureDesc MakePointShadowDepth(const TargetCapabilities& caps) noexcept;

struct PointShadowPass {
    // What one face is: a 2D target of the cube's size. Held rather than made on the
    // spot because the pass declaration points at it.
    TextureDesc faceDesc;
    TextureDesc depthFaceDesc;

    const Mesh* mesh = nullptr;
    const Pipeline* pipeline = nullptr;
    RenderPassDesc pass;

    struct PerFrame {
        // One 2D view per face of every light's cube, and the matching depth layer.
        // Kept because a frame draws through them every time; the environment bakes make
        // theirs and drop them, because those run once.
        ImageView faces[kMaxLights * 6];
        ImageView depthFaces[kMaxLights * 6];

        // **Borrowed.** main makes them and hands the same arrays to this pass and to
        // the passes that sample the cube.
        const Texture* cube = nullptr;
        const Texture* depth = nullptr;

        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    PerFrame frames[kFramesInFlight];
};

// Contract: cubeDesc outlives this pass. The declaration points at it rather than at a
//           copy, because the passes that sample the cube point at the same one and an
//           edge is the two pointers being equal.
// Contract: cubes, depths and shadows each hold kFramesInFlight entries and outlive this
//           pass. Each set names the buffer of the same index, and each cube is drawn
//           into by the frame of the same index.
bool CreatePointShadowPass(const Descriptors& descriptors,
                           const TextureDesc& cubeDesc,
                           const Texture* const cubes[kFramesInFlight],
                           const TextureDesc& depthDesc,
                           const Texture* const depths[kFramesInFlight],
                           const Mesh& mesh,
                           const Pipeline& pipeline,
                           const FramePointShadow* shadows,
                           PointShadowPass* out) noexcept;

// Input:  lights names which of the frame's lights get a cube, by index
// Effect: draws six faces for each of them, then hands the whole array to a sampler
//
// The indices are the caller's because it is the caller that decided: a point light is
// what gets one, and where those sit among the frame's lights is the scene's business.
// Every slot nobody drew is moved out of UNDEFINED anyway -- a descriptor binds the
// whole array, so a layer no shader reads still has to be in the layout that binding
// claims.
void RecordPointShadowPass(const FrameSlot& slot, const PointShadowPass& shadow,
                           const DrawList& draws,
                           const uint32_t* lights, uint32_t count) noexcept;
