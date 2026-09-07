#pragma once

// Renderer - everything a frame is drawn with, grouped by what kind of thing it is
// ============================================================================
//
// What this is, in one sentence:
//
//   Renderer consumes renderable scene state, owns its GPU resources, derives
//   rendering representations, and records their execution.
//
// Four verbs and they are not synonyms. The one that is easy to lose is the gap
// between the middle two, and three structs in Passes.h hold both sides of it:
//
//   struct FrameCamera { CameraUniform value; Buffer buffer; };
//                        ^ derived, no lifetime   ^ owned, has a destructor
//
// A view matrix is remade from state and never freed. A buffer is created once and
// must be destroyed. Owning and deriving are different obligations, which is why the
// sentence spends a verb on each.
//
// The same shape appears four times, at four rates:
//
//   asset load             CPU asset            -> GPU resource
//   renderer setup         render policy        -> pipeline, target, set layout
//   per-frame preparation  scene state          -> frame and draw representation
//   command recording      prepared state       -> vkCmd*
//
// Read down that list and it is the same arrow four times; what changes is how often
// it is drawn. It is also CLAUDE.md's "when is this value fixed" axis seen from the
// other side -- that one asks when, this one asks who.
//
// **No scene state is held here.** Counted member by member: Mesh is not geometry but
// a vertex and an index buffer, Texture is not an image file but a VkImage and a view,
// Material is not a glTF material but a descriptor set. Every one of them is a
// representation or the machinery that runs one. The single exception is the panel,
// which holds switches -- state, but the tool's rather than the scene's.
//
// What that decides, where it used to be argued:
//
//   how many lights there are     scene state    consumed, so not fixed at one here
//   where the shadow box centres  scene state    consumed -- an argument, not a constant
//   how wide that box is          technique      owned
//   near and far planes           policy         owned
//
// Three places still break the sentence, and they are the open work: main builds
// lightView and lightProj, main turns one scale into 103 model matrices, and LoadGltf
// -- which is exactly "consume an asset, make GPU resources" -- lives in main.cpp.
//
// Below, the members are grouped by the question that separates them:
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
#include "Pipelines.h"
#include "GeometryPass.h"
#include "LightingPass.h"
#include "PostProcessPass.h"
#include "ScenePass.h"
#include "Sky.h"
#include "ShadowPass.h"   // both held by value below, so the definitions have to be here
#include "Vulkan/Descriptors.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Mesh.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Texture.h"

#include <vector>

struct Renderer {
    // --- HOW to run it ----------------------------------------------------
    //
    // The programs and the pipelines, in Pipelines.h. What runs on the GPU and how is
    // that file's; what work is being done is a pass's, and the two meet only at the
    // TextureDescs both point at.
    //
    // Declared before the passes so it outlives them: a pass records through a
    // pipeline's layout, and every descriptor set was drawn from a program's.
    Pipelines pipelines;

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
    // gui's option buffer used to be the exception here, asked for through an
    // accessor because the panel was a tool rather than a dependency of the same
    // kind. **Three things stopped being true on 09-06**: the readers went from one
    // to three, each of them had to include Gui.h for that one call, and the panel
    // stopped being only a tool when it started choosing which passes run. It is a
    // FrameViewOptions above now. The edge still runs backwards through the frame,
    // and that is why UploadFrameValues asks for the value instead of being handed
    // it.
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

    // The panel's switches, and **the reason this one is here is the reason the
    // shadow map is.** It lived inside Gui until 09-06, when three passes read it and
    // each had to say GuiOptionsBuffer(gui, i) -- a name only the panel could give.
    // The buffer sits with the other three now and the panel answers with a value.
    //
    // Written by UploadFrameValues out of GuiViewUniform, not assigned from outside
    // like the three above: this edge runs backwards through the frame, from the pass
    // that draws last to the ones that draw first.
    FrameViewOptions viewOptions[kFramesInFlight];

    // The shadow map, and the first resource here that no pass owns. One pass draws
    // it and another samples it, so making it inside either would put a name only
    // that one can say -- which is how main came to reach into frames[i] for it.
    //
    // Before the passes, so it outlives both: their sets name its view.
    Texture shadowMaps[kFramesInFlight];

    // One, not one per frame in flight: it is baked once and read from then on, which
    // is what makes it an input the frame is handed rather than something the frame
    // makes. Nothing writes it after startup, so nothing can be half-written when a
    // frame reads it.
    Texture skyCube;
    Texture irradianceCube;
    Texture prefilteredCube;
    Texture brdfLut;

    // The scene's three, for the same reason and one step further: these are remade
    // on every resize, and a resize is main's to run now rather than something a pass
    // does to itself.
    SceneTargets sceneTargets[kFramesInFlight];

    // The deferred path's four, remade on the same resize and by the same rule. Both
    // chains exist at once: the panel switches which one a frame records, and a switch
    // that had to rebuild anything would not be a switch.
    GBufferTargets gbuffers[kFramesInFlight];

    ShadowPass shadowPass;
    // One per path, because the two fill different images at different sample counts.
    SkyPass skyForwardPass;
    SkyPass skyDeferredPass;

    ScenePass scenePass;

    // The other middle. Both are created, both hold their sets, and RecordFrame picks
    // -- the scene pass, or these two. What they share is everything on either side:
    // the same shadow map before, the same image after.
    GeometryPass geometryPass;
    LightingPass lightingPass;

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
