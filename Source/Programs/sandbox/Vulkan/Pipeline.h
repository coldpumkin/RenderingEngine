#pragma once

#include "Vulkan/Attachments.h"
#include "Vulkan/Device.h"
#include "Vulkan/Shader.h"
#include "Vulkan/VertexLayout.h"

// ViewportY - one sign that decides two things
// ============================================================================
//
// A negative viewport height makes the shader side y-up, and the same negation
// flips the winding test. So the sign also chooses frontFace; written by hand in
// two places they disagree silently until culling is turned on.
//
// Contract: both shaders emit triangles with positive shoelace area in clip space.
//           Measured, not derived - both pipelines run CULL_MODE_BACK.
enum class ViewportY {
    Down,   // Vulkan default, positive height
    Up,     // negative height - our world is y-up
};

// Output: the frontFace that makes the Contract triangles front-facing
constexpr VkFrontFace FrontFaceFor(ViewportY y) noexcept {
    return y == ViewportY::Up ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                              : VK_FRONT_FACE_CLOCKWISE;
}

// Output: a viewport with the sign applied. The caller never writes the sign, so it
//         cannot disagree with the frontFace baked into the pipeline.
VkViewport MakeViewport(VkExtent2D extent, ViewportY y) noexcept;

// Blending - one value, because blend and depth write cannot disagree
// ============================================================================
//
// A translucent surface with depth write on hides what is drawn behind it later.
// Two flags would make that state expressible. The cost: ordering moves to the
// recording side. Depth test stays on either way.
enum class Blending {
    Opaque,        // blend off - depth write on
    Translucent,   // blend on  - depth write off
};

// One pipeline's worth of decisions. Everything not here is the same in both of
// ours and lives in CreateGraphicsPipeline. The rows are where each value comes
// from, which is the whole reason this is a struct and not five arguments:
//
//   the mesh supplies     vertexLayout
//   the pass decides      formats . viewportY
//   the variant chooses   polygonMode . blending
//
// The shaders are not here: what they require is ShaderProgram, and several pipelines
// share one. What is left is exactly what two pipelines from one program can differ
// in, plus the two agreements a pass owns both sides of.
//
// cullMode is not here. It is dynamic state now, set at record time like the
// viewport -- see the note on VkDynamicState in the .cpp.
//
// The last row decides nothing: it carries values from Attachments so both sides of
// a baked-in contract read the same one.
struct GraphicsPipelineDesc {

    // stride 0 means no vertex buffer - the shader builds its points from
    // gl_VertexIndex.
    VertexLayout vertexLayout;

    // The same values the attachments were made from -- dynamic rendering bakes them
    // in, so a mismatch is caught at vkCmdBeginRendering. depth UNDEFINED = no depth.
    AttachmentFormats formats;

    ViewportY viewportY = ViewportY::Down;

    // FILL is the only value anything passes right now. LINE needs the device's
    // fillModeNonSolid, which we stopped requesting -- switching to it means adding
    // that back in Core.h and Device.cpp's candidate check.
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

    Blending blending = Blending::Opaque;
};

// Dynamic rendering bakes the attachment formats in. Size is not baked - viewport
// and scissor are dynamic state, so a resize rebuilds nothing.
struct Pipeline {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    // The interface this variant was built against. Borrowed: several pipelines share
    // one, which is the reason it is not in here. Recording binds sets and pushes
    // constants through program->layout, not through anything this owns.
    const ShaderProgram* program = nullptr;

    VkPipeline handle = VK_NULL_HANDLE;

    // What it was built from. Recording reads viewportY out of it, and a rebuild needs
    // the rest -- without this the caller would have to keep the desc alive.
    GraphicsPipelineDesc desc;

    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};

// Effect: destroys the compiled object and leaves the struct empty. The layout and
//         set layouts are the program's and are not touched -- that is what lets a
//         rebuild keep every set already allocated. The destructor calls this; main
//         calls it directly to rebuild in place when the surface format changes.
//
// Contract: every command buffer using this pipeline must have finished, so the
//           caller calls vkDeviceWaitIdle - one frame's fence is not enough.
void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept;


// Effect: builds one pipeline from a program and a desc, after checking both ends of
//         the shader boundary -- the layout against the vertex stage's inputs, the
//         colour formats against the fragment stage's outputs.
//
// Contract: formats must be what the attachments actually are. Nothing here can see
//           the images, so this is the one that stays a contract.
//           LINE polygonMode needs fillModeNonSolid, which is no longer requested.
bool CreateGraphicsPipeline(const VulkanDevice& dev,
                            const ShaderProgram& program,
                            const GraphicsPipelineDesc& desc,
                            Pipeline* out) noexcept;
