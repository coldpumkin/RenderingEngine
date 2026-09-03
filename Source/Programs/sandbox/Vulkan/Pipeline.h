#pragma once

#include "Vulkan/Attachments.h"
#include "Vulkan/Device.h"
#include "Vulkan/Shader.h"

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
//   the shader requires   vertPath . fragPath . vertexInput
//   the pass decides      viewportY . cullMode
//   the caller chooses    polygonMode . blending
//   passed through        colorFormat . depthFormat . samples
//
// The last row decides nothing: it carries values from Attachments so both sides of
// a baked-in contract read the same one.
struct GraphicsPipelineDesc {
    const char* vertPath = nullptr;
    const char* fragPath = nullptr;

    // nullptr means no vertex buffer - the shader builds its points from
    // gl_VertexIndex.
    const VkPipelineVertexInputStateCreateInfo* vertexInput = nullptr;

    // The same values the attachments were made from -- dynamic rendering bakes them
    // in, so a mismatch is caught at vkCmdBeginRendering. depth UNDEFINED = no depth.
    AttachmentFormats formats;

    ViewportY viewportY = ViewportY::Down;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;

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

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline handle = VK_NULL_HANDLE;

    // Read out of the shaders, like the push range. It outlives a rebuild: the sets
    // already allocated from it stay valid only while it does.
    DescriptorLayout setLayout;

    // What it was built from. Recording reads viewportY out of it, and a rebuild
    // needs the rest -- without this the caller would have to keep the desc alive.
    GraphicsPipelineDesc desc;

    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};

// Effect: destroys pipeline and layout, leaves the struct empty. setLayout is not
//         touched -- only the destructor frees that. The destructor calls this; main
//         calls it directly to rebuild in place when the surface format changes.
//
// Contract: every command buffer using this pipeline must have finished, so the
//           caller calls vkDeviceWaitIdle - one frame's fence is not enough.
void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept;


// Effect: builds one pipeline from desc. The push range, the set layout and the
//         shader stages come out of the .spv; everything else is desc. An out that
//         already carries a set layout keeps it, which is what a rebuild needs.
//
// Contract: colorFormat, depthFormat and samples must be what the attachments
//           actually are.
//           LINE polygonMode needs fillModeNonSolid, which is no longer requested.
bool CreateGraphicsPipeline(const VulkanDevice& dev,
                            const GraphicsPipelineDesc& desc,
                            Pipeline* out) noexcept;
