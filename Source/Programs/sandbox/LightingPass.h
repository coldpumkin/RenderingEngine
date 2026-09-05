#pragma once

// LightingPass - one triangle that turns the g-buffer into a picture
// ============================================================================
//
//   what              where it comes from      who else uses it
//   the g-buffer      main makes it            the geometry pass wrote it
//   the target        main makes it            the post pass samples it, and so
//                                                does the scene pass's resolve
//   the shadow map    main makes it            the shadow pass drew it, and the
//                                                scene pass reads the same one
//   camera, light,    main writes them         every pass reads the same buffers
//     shadow matrix
//   its two sets      this pass makes them     nobody
//
// **It draws into the image the scene pass resolves into.** That is what keeps the
// comparison honest downstream: whichever of the two ran, the post pass letterboxes
// the same image onto the frame's target and the gui pass draws on top of that. Only
// the middle of the frame changes.
//
// No mesh and no material. fullscreen.vert builds three points from gl_VertexIndex,
// and everything a surface was is already in the four images.
//
// Two sets, because nine bindings do not fit in the eight we allow. Set 0 is the
// frame, binding for binding what the scene pass's is; set 1 is the g-buffer. The
// split is forced by the ceiling and the place to cut is chosen so the two paths read
// one frame through one shape.
//
// **Set 0 is this pass's own even so.** The layouts look alike, but scene.vert reads
// binding 0 as well, so the scene's carries VERTEX in its stage flags where this one
// carries FRAGMENT alone -- and whether that breaks "identically defined" is a spec
// question we have not answered. An allocation is cheaper than the doubt.

#include "GeometryPass.h"   // GBufferTargets, which its second set names
#include "Gui.h"            // GuiOptionsBuffer
#include "Passes.h"

struct LightingPass {
    // loadOp DONT_CARE: one triangle covers the render area, so there is nothing to
    // preserve and nothing to clear. The post pass clears because a letterboxed draw
    // leaves bars; this draw has none.
    RenderPassDesc pass;

    // What it reads and what it writes, one per frame in flight. Non-owning both ways:
    // main makes the g-buffer and main makes the target.
    const GBufferTargets* source[kFramesInFlight]{};
    const Texture* target[kFramesInFlight]{};

    const Pipeline* pipeline = nullptr;

    // Set 0 names buffers and the shadow map, none of which a resize touches. Set 1
    // names the four g-buffer views, every one of which a resize replaces -- so a
    // resize refreshes the second array and leaves the first alone.
    VkDescriptorSet frameSets[kFramesInFlight]{};
    VkDescriptorSet gbufferSets[kFramesInFlight]{};
};

// Effect: rewrites each g-buffer set to name its four images again
//
// The pointers in source[] survive a resize -- the Textures stay where they are and
// their contents are replaced -- but the view handles inside do not, and a set records
// a handle. The same reason RefreshPostProcessPass exists, for the same event.
void RefreshLightingPass(const Descriptors& descriptors, LightingPass* lighting) noexcept;

// Effect: draws both of this pass's sets per frame and fills them
//
// Contract: source[i], target[i] and shadowMaps[i] are created and outlive this pass,
//           and all three are the ones frame i uses. A set naming another slot's would
//           read what the GPU is still writing.
// Contract: cameras, lights and shadows hold kFramesInFlight entries. shadows is the
//           same array the shadow pass was given, which is what makes the matrix in
//           binding 2 the one that drew the map in binding 3.
// Contract: gui must already be created -- binding 4 names the buffer its checkboxes
//           write into, including which g-buffer image to show.
bool CreateLightingPass(const Descriptors& descriptors,
                        const GBufferTargets* const source[kFramesInFlight],
                        const Texture* const target[kFramesInFlight],
                        const Pipeline& pipeline,
                        const Texture* const shadowMaps[kFramesInFlight],
                        const FrameCamera* cameras, const FrameLight* lights,
                        const FrameShadow* shadows,
                        const Gui& gui, LightingPass* out) noexcept;

// Input:  the pass and the slot
// Effect: makes this slot's four g-buffer images readable, then draws one triangle
//         over its target
//
// No target argument, unlike the post pass: what this draws into is one of a fixed set
// of images the pass was given, picked by slot.index, rather than whichever swapchain
// image the presentation engine handed back.
//
// The target is left COLOR_ATTACHMENT_OPTIMAL, which is exactly what the scene pass
// leaves its resolve as. That sameness is what lets the post pass not know which of
// the two ran.
void RecordLightingPass(const FrameSlot& slot, const LightingPass& lighting) noexcept;
