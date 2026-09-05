#pragma once

#include "Vulkan/Attachments.h"
#include "Vulkan/Device.h"
#include "Vulkan/Shader.h"
#include "Vulkan/VertexLayout.h"

// Pipeline - when each value is settled
// ============================================================================
//
//   compiled in        program . vertexLayout . topology . formats . samples
//                      . polygonMode . blending
//   declared dynamic   dynamicStates[], at creation
//   issued per draw    raster, by BindPipeline
//
// The split is not how often a value changes. It is whether the driver has to compile
// something different: a register costs a command, anything the machine code depends on
// costs a pipeline.

// A negative viewport height makes the shader side y-up, and the same negation flips
// the winding test -- so one sign decides both. Written by hand in two places they
// disagree silently until something is culled.
//
// Contract: both shaders emit triangles with positive shoelace area in clip space.
//           Measured, not derived - the scene pass runs CULL_MODE_BACK.
enum class ViewportY {
    Down,   // Vulkan default, positive height
    Up,     // negative height - our world is y-up
};

// Output: the frontFace that makes the Contract triangles front-facing
constexpr VkFrontFace FrontFaceFor(ViewportY y) noexcept {
    return y == ViewportY::Up ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                              : VK_FRONT_FACE_CLOCKWISE;
}

// Output: a viewport covering area, with the sign applied
VkViewport MakeViewport(VkRect2D area, ViewportY y) noexcept;

// Every dynamic state this program uses fits in this. A ceiling we impose, not a
// counted value.
constexpr uint32_t kMaxDynamicStates = 8;

// What a pipeline issues for the states it declared dynamic. **Vulkan remembers none of
// them across a command buffer**, so every one is set before every draw -- which is why
// this is a value with defaults rather than a list each pass names.
//
// The defaults are what a pass drawing one flat thing wants: no culling, no depth.
struct RasterState {
    // One field, because the viewport's sign and the winding test have to agree.
    ViewportY viewportY = ViewportY::Down;

    // The starting value. The scene pass changes it per draw, from the material.
    VkCullModeFlags cull = VK_CULL_MODE_NONE;

    // Off means nothing is hidden and the draw order is what survives. depthWrite is
    // independent, but the spec makes it irrelevant while the test is off.
    VkBool32 depthTest = VK_FALSE;
    VkBool32 depthWrite = VK_FALSE;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;   // clear is 1.0, so nearer wins

    // Everything up to the rasterizer still runs; nothing after it does.
    VkBool32 rasterizerDiscard = VK_FALSE;
};

struct Pipeline;   // defined below: this call needs only its address

// Effect: binds the pipeline and issues every state it declared dynamic, in one call
//
// area is not the pipeline's: which states are dynamic is settled at creation, what the
// viewport covers is the frame's. Three passes hand in the whole target; the post pass
// hands in a letterboxed rect, which is why this takes a rect at all.
//
// Contract: 3D -> 2D. viewport.width / |viewport.height| equals the aspect the
//           projection behind these primitives was built with. The shadow pass holds it
//           by being square, the scene pass by proj and area both reading kRenderExtent.
//
// Contract: 2D -> 2D. A pass drawing an image owes the same between what it samples and
//           what it draws into. The post pass is the only one, and holds it with
//           LetterboxInto.
//
// Neither is checked, and either one broken is a stretched picture that says nothing.
void BindPipeline(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                  const Pipeline& pipeline, VkRect2D area) noexcept;

// The same, with values from somewhere else. Separate rather than a defaulted argument,
// so a pass with nothing to override cannot name one by accident. The scene pass is the
// only caller; what it hands in is the panel's switches.
void BindPipeline(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                  const Pipeline& pipeline, VkRect2D area,
                  const RasterState& instead) noexcept;

// Compiled into the fragment output stage, so a second value is a second pipeline.
//
// Contract: a pass binding a Translucent pipeline leaves depthWrite off. Depth write is
//           dynamic state, so nothing here holds the two together.
enum class Blending {
    Opaque,
    Translucent,
};

// Everything one pipeline is created from, and the reason this is a struct rather than
// arguments is that the rows come from different places:
//
//   main supplies       program . vertexLayout . targets
//   the pass decides    topology . polygonMode . blending . raster . dynamicStates
//
// The sample count is not a field. It sits inside AttachmentFormats, projected from
// targets -- a field of its own would make "the attachment is 4x and the pipeline is
// 1x" expressible.
struct GraphicsPipelineDesc {

    // Borrowed: several pipelines share one, which is what makes the scene's fill and
    // line variants two pipelines and not two programs.
    const ShaderProgram* program = nullptr;

    // stride 0 means no vertex buffer - the shader builds its points from
    // gl_VertexIndex.
    VertexLayout vertexLayout;

    // Compiled in. VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY exists, but without
    // extendedDynamicState3 it only moves inside a class -- list to strip, not triangles
    // to lines. Tried, and the validation layer said so at the first draw.
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // What it draws into. One list, because each desc's usage already says whether it is
    // a colour or a depth target: colour order is the list's order, depth has no
    // position, and the first null ends it. One longer than the colour ceiling.
    //
    // Pointers, read during creation only -- a resize remakes these descs at a new
    // extent and rebuilds no pipeline, so Pipeline keeps the projection instead.
    //
    // An empty list is a pipeline that draws nowhere, which CheckOutputInterface refuses
    // unless the fragment stage writes nothing.
    const TextureDesc* targets[kMaxColorTargets + 1]{};

    // Contract: LINE needs the device's fillModeNonSolid, which we stopped requesting --
    //           switching to it means adding that back in Core.h and Device.cpp.
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

    Blending blending = Blending::Opaque;

    // What every state below is filled with -- baked into the pipeline, or issued by
    // BindPipeline if dynamicStates names it. One value serves both answers.
    RasterState raster;

    // Which of them the driver leaves as registers. Per desc rather than fixed for the
    // file, because a pipeline that bakes its cull mode and one that sets it per draw
    // are two different pipelines.
    //
    // The two at the front are not really optional: a baked viewport needs a VkViewport
    // at creation, and the extent is not known until there is a swapchain.
    //
    // Contract: a dynamic state must be set before every draw with this pipeline.
    //           BindPipeline issues exactly this list, which is why it is the same list.
    VkDynamicState dynamicStates[kMaxDynamicStates] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
    };
    uint32_t dynamicCount = 8;
};

struct Pipeline {
    const VulkanDevice* dev = nullptr;   // non-owning, needed to destroy

    // Borrowed, and shared between variants. Recording binds sets and pushes constants
    // through program->layout, not through anything this owns.
    const ShaderProgram* program = nullptr;

    VkPipeline handle = VK_NULL_HANDLE;

    // What it was created with, kept so callers stop carrying their own: a copy beside a
    // pipeline is a second value that can disagree with what was baked, and after the
    // fact there is nothing to check it against. Not the desc, which points at
    // TextureDescs whose extents move.
    VertexLayout vertexLayout;
    AttachmentFormats formats;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    Blending blending = Blending::Opaque;
    RasterState raster;
    VkDynamicState dynamicStates[kMaxDynamicStates]{};
    uint32_t dynamicCount = 0;

    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};

// Effect: destroys the compiled object and leaves the struct empty. The layout and set
//         layouts are the program's and are not touched, so a rebuild would keep every
//         set already allocated.
//
// Contract: every command buffer using this pipeline must have finished, so the caller
//           calls vkDeviceWaitIdle - one frame's fence is not enough.
void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept;


// Effect: builds one pipeline from the desc, after checking both ends of the shader
//         boundary -- the layout against the vertex stage's inputs, the colour formats
//         against the fragment stage's outputs.
//
// Contract: formats must be what the attachments actually are. Nothing here can see the
//           images, so this is the one that stays a contract.
bool CreateGraphicsPipeline(const VulkanDevice& dev,
                            const GraphicsPipelineDesc& desc,
                            Pipeline* out) noexcept;
