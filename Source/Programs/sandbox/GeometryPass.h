#pragma once

// GeometryPass - the same draws as the scene pass, writing facts instead of colour
// ============================================================================
//
//   what              where it comes from      who else uses it
//   its four images   main makes them          the lighting pass samples all four
//   camera            main writes it           every other pass reads the same one
//   the panel's       main makes it            the scene pass reads the same buffer
//     switches          the gui writes it        and answers three more of them
//   its one set       this pass makes it       nobody
//
// The scene pass answers "what colour is this pixel". This one answers "what is here"
// and leaves the colour to the pass after it. Same mesh, same draw list, same
// materials, same sort order -- what changes is where the answer goes.
//
// **Three of the panel's six lighting switches are this pass's.** normal map, base
// colour and alpha mask describe what a surface is, so they are decided here and
// written into the images; specular, shadow and metal/rough describe how it is lit and
// belong to the lighting pass. The forward path has all six in one shader and the line
// does not exist there at all.
//
// Its set 0 is not the scene's. It reads binding 0 and binding 4 of that layout and
// nothing between them, so BuildSetLayout leaves 1..3 out and the result is a
// different layout with the same numbering.

#include "Passes.h"

// What the geometry pass draws into. Four images, and the depth is one of them rather
// than a private scratch: the lighting pass rebuilds a world position out of it, which
// is what lets the three colour ones drop position entirely.
//
// One sample throughout, and this is the honest limit of the comparison. A multisample
// G-buffer would have to light every sample separately -- a normal averaged across an
// edge belongs to no surface, so resolving before the lighting is not available the way
// it is for the forward path. Four images times four samples is the other half of the
// price. What the two paths compare is the structure; the edges will differ.
struct GBufferTargetDescs {
    TextureDesc albedo;     // rgb the surface colour, a unused
    TextureDesc normal;     // rgb the world normal folded into 0..1
    TextureDesc material;   // r metallic, g roughness
    TextureDesc depth;      // what the position is rebuilt from
};

// Output: the four, from one size and what the device answered
//
// albedo is SRGB and the other two are UNORM, and **this is the one decision here a
// shader cannot check.** A shader sees a vec4 either way: what SRGB means is that the
// hardware converts on read and write, which is right for a colour and wrong for a
// normal or a number. Get it backwards and nothing reports anything.
//
// The sample count is not taken from caps: see the note above. depth carries SAMPLED
// as well as ATTACHMENT because it is read afterwards, which is the same kind of edge
// the shadow map's SAMPLED is.
GBufferTargetDescs MakeGBufferTargets(VkExtent2D extent, VkFormat albedo,
                                      const TargetCapabilities& caps) noexcept;

// Field for field with the descs above, which is what it is the product of. Owned by
// whoever declares one -- main does -- and both deferred passes borrow it.
struct GBufferTargets {
    Texture albedo;
    Texture normal;
    Texture material;
    Texture depth;
};

bool CreateGBufferTargets(const VulkanDevice& dev, const GBufferTargetDescs& descs,
                          GBufferTargets* out) noexcept;

// Effect: remakes the four at a new size
//
// Releases first, view before image inside each -- see ResetTexture. The caller waits
// for the GPU. **The lighting pass's second set names these four**, so it has to be
// refreshed afterwards, the way the post pass is when the resolve is remade.
bool ResizeGBufferTargets(const VulkanDevice& dev, const GBufferTargetDescs& descs,
                          GBufferTargets* out) noexcept;

// Two pipelines for one program, the way the scene pass has two: polygonMode is
// compiled in. Without the second one the panel's wireframe switch would quietly do
// nothing on this path, which is the failure this whole comparison exists to avoid.
struct GeometryPass {
    const Mesh* mesh = nullptr;

    // storeOp STORE on all four, where the scene pass is DONT_CARE on both of its.
    // That is the difference between the two passes stated as attachment ops: this
    // one's product is read by the pass after it, including the depth.
    RenderPassDesc pass;

    const Pipeline* pipeline = nullptr;
    const Pipeline* wirePipeline = nullptr;

    struct PerFrame {
        const GBufferTargets* targets = nullptr;   // borrowed. main owns them
        VkDescriptorSet set = VK_NULL_HANDLE;      // camera and the panel
    };
    PerFrame frames[kFramesInFlight];
};

// Effect: points the pass at what it draws into, and makes the set each frame binds
//
// Contract: cameras holds kFramesInFlight entries and outlives this pass.
// Contract: views holds kFramesInFlight entries and outlives this pass. Binding 4 of
//           each set names its buffer.
bool CreateGeometryPass(const Descriptors& descriptors,
                        const GBufferTargetDescs& descs,
                        const GBufferTargets* const targets[kFramesInFlight],
                        const Mesh& mesh,
                        const Pipeline& pipeline, const Pipeline& wirePipeline,
                        const FrameCamera* cameras,
                        const FrameViewOptions* views, GeometryPass* out) noexcept;

// Input:  the pass, the slot, this frame's list, what the panel decided, and where to
//         count what the recording cost
// Effect: appends the commands that fill this slot's four images
//
// The same loop as RecordScenePass, counted into the same DrawStats: binding a
// material and changing the cull mode cost what they cost whichever pass does it, and
// the two numbers are meant to be read side by side.
void RecordGeometryPass(const FrameSlot& slot, const GeometryPass& geometry,
                        const DrawList& draws, RasterOptions raster,
                        DrawStats* stats) noexcept;
