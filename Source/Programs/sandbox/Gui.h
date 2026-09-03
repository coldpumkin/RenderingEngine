#pragma once

// Gui - one panel, and the pass that draws it
// ============================================================================
//
// Beside Passes, not under Vulkan/: a panel is ours. The dependency runs the same
// way -- this names Vulkan types, and nothing under Vulkan/ names this.
//
// It is a pass, which is why it is not a parameter of the post-process one: it is a
// second set of draws into the same target, in its own BeginRendering scope with
// loadOp LOAD. Making it an argument would have put "is there a UI" inside a function
// whose whole job is to not know what it draws into.
//
// The panel exists because the toggles ran out of keys. Four fit on 1..4; the next
// thing to switch off would need a fifth nobody remembers.

#include "Vulkan/Descriptors.h"
#include "Vulkan/Device.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Instance.h"
#include "Vulkan/Mesh.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Texture.h"

struct Window;

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
};

// ImGui keeps its state in a global context, so this holds only what we own and must
// destroy. One instance; a second would fight over that context.
struct Gui {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    // ImGui's own, not the one in Descriptors. It requires
    // FREE_DESCRIPTOR_SET_BIT and ours deliberately does not have it -- we draw sets
    // once and keep them, so there is nothing to give back.
    VkDescriptorPool pool = VK_NULL_HANDLE;

    bool started = false;   // whether the backends need shutting down

    Gui() = default;
    ~Gui();
    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;
};

// Effect: starts ImGui and both backends
//
// Input: targetFormat is what the pass draws into -- the swapchain's, since the panel
//        goes on top of the finished picture rather than into the render target.
//
// Contract: the window must outlive this. ImGui's GLFW backend installs callbacks on
//           it and chains to whatever was there, which is our resize handler.
bool CreateGui(const VulkanInstance& inst, const VulkanDevice& dev,
               Window& window, VkFormat targetFormat, Gui* out) noexcept;

// What the panel reads. One struct rather than a growing argument list, and every
// field is borrowed -- main fills it each frame from things it already holds.
//
// The pipelines are here for their set layouts. Those are the only description of the
// shader interface that exists at runtime: the .spv is gone, and Vulkan will not
// answer a question about a VkDescriptorSetLayout once it is made. Same for the
// pool's sizes, which is why Descriptors keeps them.
struct GuiFrameInfo {
    float frameSeconds = 0.0f;
    uint32_t drawCount = 0;
    uint32_t materialCount = 0;

    const Descriptors* descriptors = nullptr;
    const Pipeline* scenePipeline = nullptr;
    const Pipeline* presentPipeline = nullptr;

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
// state is decided, not where commands are written -- the same line the uniform is
// on. Nothing here touches the GPU.
void BuildGui(ViewOptions* options, const GuiFrameInfo& info) noexcept;

// Effect: appends the panel's draws to the slot's command buffer
//
// Contract: BuildGui must have run this frame, and the target must already be
//           COLOR_ATTACHMENT_OPTIMAL -- the post-process pass leaves it that way.
void RecordGuiPass(const FrameSlot& slot, const Texture& target) noexcept;
