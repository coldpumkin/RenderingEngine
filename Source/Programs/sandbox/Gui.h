#pragma once

// Gui - one panel, and the pass that draws it
// ============================================================================
//
// Beside Passes, not under Vulkan/: a panel is ours. The dependency runs the same
// way -- this names Vulkan types, and nothing under Vulkan/ names this.
//
// **We draw it ourselves.** ImGui ships a Vulkan backend and it was used at first;
// it brought its own pipeline, its own descriptor pool, its own function loader, and
// asked for a MinImageCount and an ImageCount that were not promises about our
// swapchain. Everything it wanted, we already had -- so what is left of ImGui here
// is what it is actually good at: turning a widget call into a list of triangles.
//
// It is a pass, which is why it is not a parameter of the post-process one: it is a
// second set of draws into the same target, in its own BeginRendering scope with
// loadOp LOAD. Making it an argument would have put "is there a UI" inside a function
// whose whole job is to not know what it draws into.
//
// It is also the first thing here whose vertices change every frame. A mesh is
// uploaded once into DEVICE_LOCAL memory through a staging buffer; these are written
// straight into HOST_VISIBLE memory at record time, one buffer per frame in flight,
// because the CPU rewrites them while the GPU may still read the previous frame's.

#include "Config.h"   // kFramesInFlight sizes the per-frame buffers
#include "Vulkan/Buffer.h"
#include "Vulkan/Commands.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Device.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Instance.h"
#include "Vulkan/Mesh.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Texture.h"

struct Window;

// The layout for ImGui's vertex -- stride 20, and the colour is four bytes rather
// than four floats. Handed to GraphicsPipelineDesc::vertexInput the way VertexInput()
// is for the scene.
//
// A second vertex type is a second layout and nothing else. The two never meet: each
// is baked into its own pipeline.
VertexLayout GuiVertexInput() noexcept;

// Pixels to clip space. No camera and no model matrix -- the panel is already in
// window pixels, and this is the whole of an orthographic screen-space transform.
// 16 bytes where a mat4 would be 64.
//
// Contract: matches the push_constant block in gui.vert.
struct GuiPushConstants {
    float scale[2];
    float translate[2];
};

// What the panel switches off, and what the shader reads. One struct so the two
// cannot drift: main owns it, the panel edits it, the uniform copies it.
//
// bool here and float in SceneUniform -- GLSL has no bool in a uniform block worth
// using, and the conversion is one place.
struct ViewOptions {
    bool normalMap = true;
    bool baseColor = true;
    bool specular = true;
    bool alphaMask = true;
    bool shadow = true;

    // The three below do not reach a shader. They are read on the CPU where the scene
    // pass records, and they differ in what that costs:
    //
    //   wireframe   polygonMode is compiled in, so it selects between two pipelines
    //   depthTest   dynamic state, one command, no pipeline
    //   cull        dynamic state, and one the material already sets per draw
    //
    // Which is which is not about how often they change -- all three change never, or
    // when a checkbox moves. It is about whether the driver has to compile something
    // different.
    bool wireframe = false;

    // Off, the depth test stops hiding anything and the draw order becomes visible:
    // what is on screen is whatever was recorded last. That is the order main sorted
    // into, made looking at it possible.
    bool depthTest = true;

    // Material means each draw keeps the cull mode its glTF material asked for, which
    // is what the sort key groups by. The other three override every draw, and Front
    // is the one worth having -- it shows the inside of a closed surface.
    enum class CullChoice { Material, None, Back, Front };
    CullChoice cull = CullChoice::Material;
};

// The same switches as the shader reads them.
//
// Here rather than in SceneUniform, which is where they used to sit. What they answer
// to is the panel, not the scene: nothing about a camera or a light decides them, and
// the pass whose uniform carried them had no say in any of it. Moving them out left
// SceneUniform with only values the frame actually computes.
//
// Floats rather than a bitfield: std140 packs them into whole vec4s either way, and
// this way each has a name on both sides of the boundary instead of a bit position.
// 0 or 1, and the shader compares against 0.5 so a half value is not a third state.
//
// Contract: field order matches the View block in scene.frag.
struct ViewOptionsUniform {
    float normalMap;
    float baseColor;
    float specular;
    float alphaMask;
    float shadow;
    float pad[3];   // std140 rounds the block up to a second vec4
};

// ImGui keeps its widget state in a global context, so this holds only what we own
// and must destroy. One instance; a second would fight over that context.
struct Gui {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    // The atlas ImGui bakes its glyphs into, as one of our textures. Uploaded once:
    // the picture never changes, only which parts of it a frame reads.
    Texture font;
    VkDescriptorSet set = VK_NULL_HANDLE;   // drawn from our pool, names font

    // Non-owning, like the passes' own. Held so recording takes the same shape as
    // theirs: the pass knows these, the caller does not carry them.
    //
    // Two, for the reason the other passes hold two: the program is the interface
    // every pipeline here would share, the pipeline is the one variant.
    const ShaderProgram* program = nullptr;
    const Pipeline* pipeline = nullptr;

    // Written at record time, so one pair per frame in flight. Fixed size: growing
    // them would be a heap allocation in the frame loop, which this program does not
    // do. A frame that does not fit is skipped, and says so once.
    struct PerFrame {
        Buffer vertices;
        Buffer indices;

        // What the panel decided, in the form the scene's shaders read. Per frame in
        // flight like everything else here: the CPU writes this one while the GPU may
        // still be reading the previous frame's.
        //
        // Owned by the panel and read by another pass, which is the one direction
        // nothing else here runs in -- so the scene pass names it through a function
        // below rather than reaching into this struct.
        Buffer options;
    };
    PerFrame frames[kFramesInFlight];

    // Edited by the checkboxes, copied into frames[i].options once a frame. Not the
    // caller's any more: main used to declare it, hand it to BuildGui and then copy
    // every field into the scene's uniform by hand.
    ViewOptions options;

    bool started = false;       // whether the context needs destroying
    bool warnedTooBig = false;  // so that message cannot flood

    Gui() = default;
    ~Gui();
    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;
};

// Effect: starts ImGui and its GLFW input backend, bakes the font atlas into a
//         texture, and makes the per-frame vertex and index buffers
//
// Before the descriptor pool, because the pool has to be told about this pass's set
// before it is created.
//
// Contract: the window must outlive this. ImGui's GLFW backend installs callbacks on
//           it and chains to whatever was there, which is our resize handler.
bool CreateGui(const VulkanDevice& dev, const Commands& commands,
               Window& window, Gui* out) noexcept;

// Effect: draws the one set this pass needs and points it at the font
//
// Separate from the above because a set cannot exist before the pool, and the pool
// cannot be sized before every pass has said what it wants.
bool CreateGuiSet(const Descriptors& descriptors, const ShaderProgram& program,
                  const Pipeline& pipeline,
                  Gui* out) noexcept;

// What the panel reads. One struct rather than a growing argument list, and every
// field is borrowed -- main fills it each frame from things it already holds.
//
// The pipelines are here for their set layouts. Those are the only description of the
// shader interface that exists at runtime: the .spv is gone, and Vulkan will not
// answer a question about a VkDescriptorSetLayout once it is made. Same for the
// pool's sizes, which is why Descriptors keeps them.
struct GuiFrameInfo {
    float frameSeconds = 0.0f;

    // The list's length, and the array's. Neither is a count of what happened: an
    // item whose material index is out of range is skipped.
    uint32_t itemCount = 0;
    uint32_t materialCount = 0;

    // What recording that list actually cost, from the last frame that recorded one --
    // the panel is built before RecordFrame runs, and the list does not change between
    // frames. Zero until the first frame is through.
    //
    // Three numbers rather than the DrawStats they were copied from, for the reason
    // the textures below are not the pass they belong to: this header names Vulkan
    // types and nothing of ours above it.
    uint32_t recordedDraws = 0;
    uint32_t materialBinds = 0;
    uint32_t cullChanges = 0;

    const Descriptors* descriptors = nullptr;

    // Both halves, because the panel shows both: what the shaders require (the
    // program's set layouts) and what one variant baked (the pipeline's desc).
    const ShaderProgram* sceneProgram = nullptr;
    const ShaderProgram* presentProgram = nullptr;
    const ShaderProgram* guiProgram = nullptr;
    const Pipeline* scenePipeline = nullptr;
    const Pipeline* presentPipeline = nullptr;
    const Pipeline* guiPipeline = nullptr;

    // sizeof on our side of the boundary. The shader's side is in the .spv and the
    // two are checked only where the pipeline was built.
    uint32_t uniformBytes = 0;
    uint32_t pushBytes = 0;
    uint32_t vertexStride = 0;
    uint32_t vertexAttributes = 0;
    uint32_t framesInFlight = 0;

    // This frame's slot, and the images it draws through in order. Textures rather
    // than the pass they belong to: a Texture is a Vulkan/ type, and keeping the
    // panel on that side of the line means it still knows nothing about a pass.
    uint32_t slotIndex = 0;
    const Texture* sceneColor = nullptr;     // multisample, discarded
    const Texture* sceneResolve = nullptr;   // 1 sample, what leaves the scene pass
    const Texture* sceneDepth = nullptr;     // multisample, never leaves the frame
    const Texture* frameTarget = nullptr;    // the acquired swapchain image

    const Mesh* mesh = nullptr;
};

// Effect: builds this frame's widgets and leaves them ready to record
//
// Input/Output: options, edited in place by the checkboxes
//
// Separate from the recording below because it runs where the rest of the frame's
// state is decided, not where commands are written. Nothing here touches the GPU.
void BuildGui(Gui* gui, const GuiFrameInfo& info) noexcept;

// Output: the buffer the scene pass's set should name for this frame, and its size
//
// A function rather than a reachable field: what the scene pass needs is one handle
// per frame, and this is the whole of what it may know about the panel.
//
// Contract: gui must already be created -- CreateScenePass fills its sets once, and
//           a null handle there is a validation error at bind time.
VkBuffer GuiOptionsBuffer(const Gui& gui, uint32_t frameIndex) noexcept;
constexpr VkDeviceSize kGuiOptionsSize = sizeof(ViewOptionsUniform);

// The three CPU-side answers the scene pass needs. Functions for the same reason
// GuiOptionsBuffer is one: what the pass needs is an answer, and these are the whole
// of what it may know about the panel.
bool GuiWireframe(const Gui& gui) noexcept;
bool GuiDepthTest(const Gui& gui) noexcept;

// Output: the cull mode to use for every draw, or UINT32_MAX to leave it to each
//         material. The sentinel is outside VkCullModeFlagBits, so no real value
//         collides with it.
constexpr VkCullModeFlags kCullFromMaterial = UINT32_MAX;
VkCullModeFlags GuiCullMode(const Gui& gui) noexcept;

// Effect: copies this frame's switches into the buffer the scene pass will read
//
// Called where the other passes' uniforms are copied, and before any of them record:
// the scene pass samples this in the same submission.
void UploadGuiOptions(const Gui& gui, uint32_t frameIndex) noexcept;

// Effect: copies this frame's vertices into its buffers and appends the panel's draws
//
// Contract: BuildGui must have run this frame, and the target must already be
//           COLOR_ATTACHMENT_OPTIMAL -- the post-process pass leaves it that way.
void RecordGuiPass(const FrameSlot& slot, Gui& gui, const Texture& target) noexcept;
