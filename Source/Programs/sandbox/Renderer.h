#pragma once

// Renderer - everything a frame is drawn with, grouped by what kind of thing it is
// ============================================================================
//
// main declared these one after another in destruction order, which says when each
// dies and nothing about what it is. Here they are grouped by the question that
// actually separates them:
//
//   HOW to draw     baked into a pipeline at creation. Never changes after
//   WHAT it reaches descriptor pool and sampler -- the rules, not the data
//   WHAT to draw    uploaded once, read every frame
//   WHERE it goes   attachments, sets naming them, and the passes that use them
//   WHEN it runs    one slot per frame in flight
//
// The line against main is device ownership: everything here needs a VkDevice to
// exist, and everything main keeps is what makes one. So main still holds the window
// system, the instance, the device, the surface and the command pools.
//
// **Why now.** The panel needed all of it. GuiFrameInfo was thirteen lines of the
// frame loop gathering these by hand every frame, which is the trigger CLAUDE.md
// wrote down for this: something outside main wanting the resources whole.
//
// Destruction order is checked, not assumed. Members die in reverse, and the two
// orders happen to agree here:
//
//   scenePass points at mesh          -> mesh declared first, dies later     OK
//   passes hold sets from the pool    -> descriptors declared first          OK
//   passes borrow their pipelines     -> pipelines declared first            OK
//
// A regrouping that breaks one of those is a compile-time silence and a runtime
// crash, so the list above is a contract and not a note.

#include "Config.h"
#include "Gui.h"
#include "Passes.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Mesh.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Texture.h"

#include <vector>

struct Renderer {
    // --- HOW to draw ------------------------------------------------------
    //
    // Three pipelines, and what separates them is not "three passes" -- the scene
    // pass could use several and the gui pass happens to use one. What separates
    // them is that each pair of shaders declares a different interface:
    //
    //   pipeline   shaders                vertex        target        blend
    //   scene      mesh.vert/frag         Vertex (48)   color 4x      opaque
    //   present    fullscreen.vert/frag   none          swapchain 1x  opaque
    //   gui        gui.vert/frag          ImDrawVert    swapchain 1x  translucent
    //
    // The empty middle cell is the interesting one: a shader that builds its own
    // vertices needs no layout at all, and that is a property of the shader rather
    // than of the pass it happens to be in.
    Pipeline scenePipeline;
    Pipeline presentPipeline;
    Pipeline guiPipeline;

    // --- WHAT the shaders may reach ---------------------------------------
    //
    // The pool and the sampler. Not the sets: those belong to whoever fills them,
    // because filling one needs that owner's resources.
    Descriptors descriptors;

    // --- WHAT to draw -----------------------------------------------------
    //
    // Uploaded once, read every frame. One mesh holds every primitive end to end and
    // a DrawItem is a span inside it, which is why 103 draws rebind nothing.
    //
    // textures outlives materials on purpose: a Material is a set naming two of
    // these, and the set is read until the pool goes.
    Mesh mesh;
    std::vector<Texture> textures;
    std::vector<Material> materials;

    // --- WHERE a frame goes -----------------------------------------------
    //
    // Three passes in the order RecordFrame runs them. What is not written down
    // anywhere is the edge between them: postPass reads what scenePass wrote, and
    // it says so by walking a path (source->frames[i].colorResolve) rather than by
    // naming a resource both of them know. That missing name is the open trigger
    // in CLAUDE.md.
    ScenePass scenePass;
    PostProcessPass postPass;
    Gui guiPass;

    // --- WHEN it runs -----------------------------------------------------
    //
    // A slot is what a frame executes on, counted by frames in flight. Its opposite
    // number -- where the frame goes, counted by swapchain images -- is not here:
    // FrameTarget is made per acquire and lives no longer than the frame.
    FrameSlot slots[kFramesInFlight];

    Renderer() = default;
    ~Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
};
