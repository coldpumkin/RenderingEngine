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
#include "PostProcessPass.h"
#include "ShadowPass.h"   // both held by value below, so the definitions have to be here
#include "Vulkan/Descriptors.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Mesh.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Texture.h"

#include <vector>

struct Renderer {
    // --- WHAT the shaders require -----------------------------------------
    //
    // Three programs, one per pair of shaders. Each is the interface: set layouts,
    // push range, pipeline layout -- everything read out of the .spv and nothing
    // chosen by a caller.
    //
    //   program   shaders                vertex          sets
    //   shadow    shadow.vert/frag       position (48)   0 the light's matrix
    //   scene     scene.vert/frag         Vertex (48)     0 frame, 1 material
    //   post      fullscreen.vert + post.frag   none            0 the scene's resolve
    //   gui       gui.vert/frag          ImDrawVert      0 the font atlas
    //
    // The shadow row reads the same buffer over the same stride as the scene row and
    // declares one attribute of it. A layout feeds what its shader reads, and this one
    // reads position.
    //
    // The empty middle cell is the interesting one: a shader that builds its own
    // vertices needs no layout at all, and that is a property of the shader rather
    // than of the pass it happens to be in.
    //
    // Declared first, so they are destroyed last. Every pipeline points at one, and
    // every descriptor set was drawn from one of their layouts.
    ShaderProgram shadowProgram;
    ShaderProgram sceneProgram;
    ShaderProgram postProgram;
    ShaderProgram guiProgram;

    // --- HOW to draw ------------------------------------------------------
    //
    // A pipeline is one variant of a program: the state a pass admits, compiled.
    // Today each program has exactly one, which is why the two look like one thing.
    //
    //   pipeline   from             target          polygon   blend
    //   shadow     shadowProgram    depth only 1x   fill      opaque
    //   scene      sceneProgram     color 4x        fill      opaque
    //   sceneWire  sceneProgram     color 4x        line      opaque
    //   post       postProgram   swapchain 1x    fill      opaque
    //   gui        guiProgram       swapchain 1x    fill      translucent
    //
    // The middle two are the table earning its keep: one program, two rows, and the
    // only column that differs is polygonMode. Every set drawn from sceneProgram fits
    // both, so the scene pass swaps between them and rebinds nothing.
    //
    // "depth only" is a colour format of UNDEFINED, and it is checked rather than
    // assumed: a fragment stage with no outputs and a pass with no colour attachment
    // have to agree, and CreateGraphicsPipeline refuses the pair that does not.
    //
    // A second scene pipeline would appear in this table and nowhere else: it shares
    // sceneProgram, so every set already allocated fits it.
    Pipeline shadowPipeline;
    Pipeline scenePipeline;
    Pipeline sceneWirePipeline;
    Pipeline postPipeline;
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
    // Four passes in the order RecordFrame runs them, which is also the reverse of
    // the order they are destroyed in -- and that is not a coincidence here, it is the
    // dependency: scenePass's sets name shadowPass's depth maps, postPass reads what
    // scenePass wrote.
    //
    // The edges between these are values now, not paths: main hands postPass the
    // images it reads, hands both light-reading passes the same lights[], and hands
    // scenePass the shadow maps rather than the pass that owns them.
    //
    // What is left is gui's option buffer, which scenePass asks for through an
    // accessor -- and which runs the other way, from a pass that draws later to one
    // that draws first. Deliberately left as it is: the panel is a tool for comparing
    // features while they are understood, not a dependency of the same kind.
    //
    // Declared before the passes so they outlive them: their sets name these buffers,
    // and members are destroyed in reverse.
    //
    // Here and not in a pass because main writes both every frame and no pass writes
    // either. What a pass owns is what it draws into.
    FrameCamera cameras[kFramesInFlight];
    FrameLight lights[kFramesInFlight];

    // Apart from lights because the two answer different questions: what reaches a
    // surface, and where the map that shadows it was drawn from. Read by two passes,
    // which is why it is out here beside them rather than inside either.
    FrameShadow shadows[kFramesInFlight];

    // The shadow map, and the first resource here that no pass owns. One pass draws
    // it and another samples it, so making it inside either would put a name only
    // that one can say -- which is how main came to reach into frames[i] for it.
    //
    // Before the passes, so it outlives both: their sets name its view.
    Texture shadowMaps[kFramesInFlight];

    // The scene's three, for the same reason and one step further: these are remade
    // on every resize, and a resize is main's to run now rather than something a pass
    // does to itself.
    SceneTargets sceneTargets[kFramesInFlight];

    ShadowPass shadowPass;
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
