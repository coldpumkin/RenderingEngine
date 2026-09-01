#pragma once

#include "Vulkan/Descriptors.h"
#include "Vulkan/RenderTargets.h"

// PushConstants holds a mat4. Header-only, so it costs nothing at link time.
#include <glm/glm.hpp>

// Viewport y direction - one decision that shows up in two places
// ============================================================================
//
// A Vulkan framebuffer has y pointing down. A negative viewport height makes the
// shader side y-up (VK_KHR_maintenance1, core in 1.1), and the same negation
// flips the winding test: front and back come from the sign of the triangle's
// area in framebuffer space, and a negative y scale flips that sign.
//
// So choosing the viewport sign also chooses frontFace. Written by hand in two
// places they can disagree, and nothing happens while culling is off - the
// screen goes empty the moment it is turned on.
//
// Contract: both shaders emit triangles whose shoelace area is positive in clip
//           space. The mapping below holds only on top of that. Counted:
//             triangle.vert    two world-space triangles, +1.0 each
//             fullscreen.vert  (-1,-1) (3,-1) (-1,3) -> +8
//
// The mapping is measured, not derived. Both pipelines run with CULL_MODE_BACK
// and check.ps1 reproduces the baseline pixel ratios.
enum class ViewportY {
    Down,   // Vulkan default, positive height
    Up,     // negative height - our world coordinates are y-up
};

// Input:  the viewport y direction this pipeline draws with
// Output: the frontFace that makes the Contract triangles front-facing
constexpr VkFrontFace FrontFaceFor(ViewportY y) noexcept {
    return y == ViewportY::Up ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                              : VK_FRONT_FACE_CLOCKWISE;
}

// Input:  extent, and the viewportY of the pipeline about to draw
// Output: a viewport with the sign applied, ready for vkCmdSetViewport
//
// The caller never writes the sign by hand, so it cannot disagree with the
// frontFace baked into the pipeline.
VkViewport MakeViewport(VkExtent2D extent, ViewportY y) noexcept;

// Opaque or translucent - one value that decides two states
// ============================================================================
//
// Translucency takes more than turning blending on: depth write has to go off
// with it. Left on, a translucent surface records its own depth and hides what
// is drawn behind it afterwards. Two separate flags would make "blend on, write
// on" expressible at all.
//
// The cost is that ordering becomes the recording side's job - opaque first,
// translucent back to front. That is the work depth was doing for us.
//
// Depth test stays on: a translucent surface behind an opaque one should be hidden.
enum class Blending {
    Opaque,        // blend off - depth write on
    Translucent,   // blend on  - depth write off
};

// Graphics pipeline - what we draw with
// ============================================================================
//
// Dynamic rendering bakes the attachment formats into the pipeline. Size is not
// baked: viewport and scissor are dynamic state, so a resize rebuilds nothing.
struct Pipeline {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline handle = VK_NULL_HANDLE;

    // frontFace was derived from this and is already baked in. Kept here so the
    // recording side can hand the same value to MakeViewport - the other half
    // of the pair, and the only value in this struct that leaves the file.
    ViewportY viewportY = ViewportY::Down;

    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};

// Effect: destroys the pipeline and its layout and leaves the struct empty.
//         The destructor calls this.
//
// The destructor alone falls short in one case: rebuilding while the owner stays
// alive. When the surface format changes, the fullscreen pipeline still has the
// old format baked in, so main destroys and rebuilds it in place. Same shape as
// DestroyImage.
//
// Contract: every command buffer referencing this pipeline must have finished.
//           With two frames in flight another frame's cmd may still be running,
//           so the caller calls vkDeviceWaitIdle first.
void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept;

// Values handed to the shader every frame. They ride inside the command buffer,
// so there is no pool, no set, no update and no lifetime to manage. The spec
// guarantees at least 128 bytes.
//
// The three matrices are multiplied on the CPU and sent as one. Sent apart they
// would be 192 bytes, past that 128 guarantee, and no shader needs them
// separately yet - lighting that wants world coordinates is what splits model
// back out.
//
// glm::mat4 is 64 bytes, column-major, and matches the GLSL mat4 layout: it
// rides as is, no transpose (ThirdParty/glm/VERSION.md has the numbers).
//
// alpha sits beside mvp because their cycle is the same - both are decided per
// object and change per draw. A different cycle would mean a different home.
//
// Contract: field order and types must match the shader's layout(push_constant)
//           block. A mismatch compiles and runs, only the values come out wrong.
//           The validation layer checks the size but not the field order, and
//           nothing else looks at both sides.
//
// Contract: every stage that reads this block must appear in
//           pushRange.stageFlags. fragment reads alpha, so VERTEX is not enough.
struct PushConstants {
    glm::mat4 mvp;   // model -> world -> view -> clip
    float alpha;     // 1.0 is opaque. Ignored by opaque pipelines: blending is off
};

// For the scene pass. Reads a vertex buffer and tests depth. viewportY = Up.
//
// polygonMode is the one argument here because the caller genuinely has a
// choice: the same vertices can be drawn as faces or as lines. The rest - vertex
// layout, push size, y-up, culling - is the scene pass convention, so it is
// decided inside where the caller cannot get it wrong.
//
// Contract: formats must be what RenderTargets actually created.
//           LINE requires the device's fillModeNonSolid (Core.h).
bool CreateTrianglePipeline(const VulkanDevice& dev,
                            RenderTargetFormats formats,
                            VkDescriptorSetLayout setLayout,
                            VkPolygonMode polygonMode,
                            Blending blending,
                            Pipeline* out) noexcept;

// For the present pass. Samples what the scene pass resolved and draws it to the
// swapchain.
//
// What differs from the triangle pipeline shows up in the contract:
//   colorFormat   the swapchain's, not our render target's
//   samples       1 - MSAA already ended in the scene pass resolve
//   depth         none
//   vertex input  none, the shader builds three points from gl_VertexIndex
//   setLayout     presentLayout - fullscreen.frag reads one sampler2D
//   viewportY     Down - the shader makes its own uv, flipping would invert it
bool CreateFullscreenPipeline(const VulkanDevice& dev,
                              VkFormat colorFormat,
                              VkDescriptorSetLayout setLayout,
                              Pipeline* out) noexcept;
