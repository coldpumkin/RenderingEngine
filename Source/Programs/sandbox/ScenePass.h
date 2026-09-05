#pragma once

// ScenePass - the off-screen pass, and the surfaces it draws
// ============================================================================
//
//   what              where it comes from      who else uses it
//   its three images  main makes them          the post pass samples the resolve,
//                                              and so does the capture
//   the shadow map    main makes it            the shadow pass drew it
//   camera, light,    main writes them         the shadow pass reads the same
//     shadow matrix     every frame              FrameShadow
//   the panel's       main makes it            the other two surface passes read
//     switches          the gui writes it        the same FrameViewOptions
//   its two sets      this pass makes them     nobody
//
// Owns nothing it draws with or into. What it owns is the material sets, which are
// what a surface looks like rather than where a frame goes.

#include "Passes.h"


// What a pass draws into, described the way every other texture here is
// ============================================================================
//
// A TextureDesc says four things and AttachmentFormats says two of them, so these are
// what a target actually is and the pipeline's value is derived from them. The two
// AttachmentFormats drops are the two that mattered all along:
//
//   extent   the aspect a projection is built with comes from here
//   usage    ATTACHMENT is what the pipeline draws into. **SAMPLED is an edge** --
//            it marks the images another pass reads, and there are exactly two
//
// Written by the caller, beside the other things it hands a pass, rather than made up
// inside pass creation from formats read back off a pipeline.
struct SceneTargetDescs {
    TextureDesc color;     // multisample. Drawn into, then discarded
    TextureDesc resolve;   // 1 sample. What leaves the pass
    TextureDesc depth;     // multisample. Never read outside the frame
};

// Output: the three, from one size, one choice and what the device answered
//
// The expansion rule, which used to be four lines inside CreateScenePass and a
// sentence in its comment. Two callers now -- creation and every resize.
//
// colour is the one field a caller decides: the render chain's format is ours, not the
// device's, so TargetCapabilities has no opinion on it. Depth and the sample count are
// the device's answers and come whole -- which is what lets the resolve refuse the
// sample count while the other two take it.
SceneTargetDescs MakeSceneTargets(VkExtent2D extent, VkFormat colour,
                                  const TargetCapabilities& caps) noexcept;

// The three images those descs describe, made together and remade together
//
// Field for field with SceneTargetDescs, because that is what it is the product of.
// Owned by whoever declares one -- main does -- and the scene pass borrows it, the
// way both passes borrow the shadow map.
struct SceneTargets {
    Texture color;     // multisample. Drawn into, then discarded
    Texture resolve;   // 1 sample. vkCmdEndRendering averages into it, and the post
                       // pass samples it -- the one that leaves
    Texture depth;     // multisample. Tested and written, never read outside the frame
};

// Effect: makes the three, or remakes them at a new size
//
// Remaking releases first, view before image inside each -- see ResetTexture. The
// caller waits for the GPU: a frame in flight is still reading last frame's, and no
// fence here says which. The post pass's sets name the resolve, so they have to be
// refreshed afterwards; nothing else does.
bool CreateSceneTargets(const VulkanDevice& dev, const SceneTargetDescs& descs,
                        SceneTargets* out) noexcept;
bool ResizeSceneTargets(const VulkanDevice& dev, const SceneTargetDescs& descs,
                        SceneTargets* out) noexcept;

// ScenePass - the off-screen pass, and what it draws into
// ============================================================================
//
// The pass is one; its attachments are one set per frame in flight. Every frame
// draws into them again, so a frame cannot share them with one the GPU has not
// finished -- the first barrier in recording is srcStage TOP_OF_PIPE, which waits
// for nothing.
//
// The mesh sits beside frames[], not inside it: nothing writes it after creation, so
// every frame reads the same one. A pointer because the scene owns it.
//
// No texture here any more. It was one because there was one, and the moment a second
// arrived it stopped being a property of the pass -- it is a Material now, and the
// DrawItem says which.
//
// What the pass owns, and what reaches it from outside
// ----------------------------------------------------------------------------
//
// Sorted by that question rather than by type, because the answer is lopsided:
//
//   owns       the images it draws into. That is the whole list.
//
//   receives   the light                  an argument, one buffer per frame
//              the shadow maps            an argument, one image per frame
//              the raster switches        an argument to RecordScenePass
//              the camera                 main assigns into frames[i] from outside
//              the panel's switches       an argument, one buffer per frame
//
// Four of the five now say what they are in the signature. The camera is the one
// that does not -- main reaches in and writes it.
//
// The panel's used to be the exception, asked for through an accessor on the ground
// that a panel is a tool rather than a dependency of the same kind. **That stopped
// holding on 09-06**: two more passes came to read it, and each had to include Gui.h
// for one call. It is a FrameViewOptions now, handed over like the light.
//
// This is written as a list and not as a type on purpose. Naming what a pass owns is
// what has to happen before anything derives from it, and a struct now would fix the
// answer while three of the four routes still have no reason to be what they are.
//
// The line is also not a partition. "The pass owns this" says nothing about what a
// draw owns, and the tempting reading -- everything else is the draw's -- would settle
// a question the asset is currently answering. The pipeline below is the case: it is
// the pass's because no material in Sponza asks for a second one, not because a pass
// is the thing that holds a pipeline.
struct ScenePass {
    const Mesh* mesh = nullptr;

    // What this pass does to its two attachments, settled once. The colour resolves:
    // storeOp DONT_CARE goes with that, because only the resolved copy is read
    // afterwards and writing the multisample image back would be pure bandwidth --
    // resolveMode is what drives the averaging, not storeOp. The depth is DONT_CARE
    // for a different reason: it is used only inside this frame.
    RenderPassDesc pass;

    // No program here. What a draw receives -- which set layouts, which push range --
    // is the pipeline's, and recording reads it off whichever variant it just chose.
    //
    // It used to be the pass's, on the grounds that several pipelines bind through one
    // layout. That is true of these two and it is not what a pass is: Vulkan admits any
    // pipeline compiled for the same attachment formats, and nothing more. Holding the
    // program here turned our recording shortcut into a rule the pass enforced.
    //
    // Two variants of the one program, and the pass picks between them at record
    // time. They differ in polygonMode and in nothing else -- same shaders, same set
    // layouts, same push range, same attachment formats.
    //
    // That sameness is the point rather than a coincidence: every descriptor set this
    // pass allocated was drawn from program's layouts, so switching between these two
    // rebinds nothing. It is what "a pipeline is one variant of a program" means when
    // there is finally more than one.
    //
    // Not on the DrawItem. Which pipeline is used is one answer for the whole pass,
    // not something a draw decides -- and this asset gives no reason for it to be:
    // 25 materials, 22 OPAQUE and 3 MASK, and MASK is a discard in the shader.
    const Pipeline* pipeline = nullptr;
    const Pipeline* wirePipeline = nullptr;

    struct PerFrame {
        // **Borrowed.** main owns them, so a resize is main's to run and the post pass
        // can be handed the resolve without anyone naming this pass.
        const SceneTargets* targets = nullptr;

        // Drawn from the pool by this pass and filled by it: the set names this
        // frame's input and uniform, so no one else knows what belongs in it.
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    PerFrame frames[kFramesInFlight];
};

// Effect: points the pass at what it draws into and at what the scene brings, and
//         makes the set each frame binds.
//
// It made the attachments until 09-05. main owns them now, which is what leaves this
// with no VulkanDevice argument: it creates descriptor sets and nothing else, and the
// pool knows its device.
//
// shadowMaps and not a ShadowPass, which was the same change made earlier: what this
// needs is one depth image per frame, and naming the pass that owns them let this
// function reach anything a shadow pass has. A signature is meant to state the
// requirement, not a place the requirement can be found in.
//
// views is a FrameViewOptions array and not a Gui, which is the same change made for
// shadowMaps just above: what this needs is one buffer per frame, and naming the
// thing that owns them let this function reach anything a panel has.
//
// Contract: shadowMaps holds kFramesInFlight entries, each an image the shadow pass
//           has created, frame for frame. Read at set-fill time and not stored: the
//           barrier that makes one readable belongs to the pass that writes it.
// Contract: views holds kFramesInFlight entries and outlives this pass. Binding 4 of
//           each set names its buffer.
// A resize touches no pass. This one's sets name nothing that changes -- the camera,
// the light, a shadow map and the panel's switches -- and it holds pointers to targets
// whose contents are replaced under them. **The post pass's sets do not survive**:
// they name the resolve image, so RefreshPostProcessPass runs after every resize.

// Contract: cameras, lights and shadows hold kFramesInFlight entries and outlive this
//           pass. shadows is the same array the shadow pass was given, which is what
//           makes the matrix in binding 2 the one that drew the map in binding 3.
bool CreateScenePass(const Descriptors& descriptors,
                     const SceneTargets* const targets[kFramesInFlight],
                     const Mesh& mesh,
                     const Pipeline& pipeline, const Pipeline& wirePipeline,
                     const Texture* const shadowMaps[kFramesInFlight],
                     const FrameCamera* cameras, const FrameLight* lights,
                     const FrameShadow* shadows,
                     const FrameViewOptions* views, ScenePass* out) noexcept;


// Input:  the pass, the slot, this frame's list, what the panel decided, and where to
//         count what the recording cost
// Effect: appends the commands that draw this slot's colour and depth
//
// One mesh for every item: the spans in items index into it. Materials are bound only
// when the one a draw needs is not the one already bound, which is what sorting the
// list by material buys.
void RecordScenePass(const FrameSlot& slot, const ScenePass& scene,
                     const DrawList& draws, RasterOptions raster,
                     DrawStats* stats) noexcept;

