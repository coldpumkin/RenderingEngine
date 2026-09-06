#pragma once

#include "Vulkan/Attachments.h"
#include "Vulkan/Device.h"
#include "Vulkan/Shader.h"
#include "Vulkan/VertexLayout.h"

// Pipeline - when each value is settled
// ============================================================================
//
//   compiled in        program . vertexLayout . topology . formats . samples
//                      . polygonMode . blend
//   declared dynamic   dynamicStates[], at creation
//   issued per draw    raster, by BindPipeline
//
// Not how often a value changes -- whether the driver has to compile something
// different. A register costs a command; anything the machine code depends on costs a
// pipeline.

// One sign for two things: a negative viewport height makes the shader side y-up, and
// the same negation flips the winding test. Written by hand in two places they disagree
// silently until something is culled.
//
// Contract: every vertex stage here emits triangles with positive shoelace area in clip
//           space. Measured, not derived -- nothing says so until one culls.
enum class ViewportY {
    Down,   // Vulkan default, positive height
    Up,     // negative height, for a y-up world
};

// Output: the frontFace that makes the Contract triangles front-facing
constexpr VkFrontFace FrontFaceFor(ViewportY y) noexcept {
    return y == ViewportY::Up ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                              : VK_FRONT_FACE_CLOCKWISE;
}

// Output: a viewport covering area, with the sign applied
VkViewport MakeViewport(VkRect2D area, ViewportY y) noexcept;

constexpr uint32_t kMaxDynamicStates = 8;   // a ceiling we impose, not a counted value

// The values a pipeline issues for the states it declared dynamic. Vulkan remembers
// none of them across a command buffer, so every one is set before every draw.
struct RasterState {
    ViewportY viewportY = ViewportY::Down;          // one field: sign and winding agree
    VkCullModeFlags cull = VK_CULL_MODE_NONE;       // a draw may change it, one command
    VkBool32 depthTest = VK_FALSE;                  // off: draw order is what survives
    VkBool32 depthWrite = VK_FALSE;                 // independent, moot while test is off
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;  // clear is 1.0, so nearer wins
    VkBool32 rasterizerDiscard = VK_FALSE;          // vertex work runs, nothing lands
};

struct Pipeline;   // defined below: this call needs only its address

// Effect: binds the pipeline and issues every state it declared dynamic, in one call
//
// area is not the pipeline's. A rect rather than an extent because a caller may draw
// into less than the whole target, and the bars are the difference.
//
// Contract: viewport.width / |viewport.height| equals the aspect the projection behind
//           these primitives was built with. A caller drawing an image rather than
//           geometry owes the same between what it samples and what it draws into.
//           Neither is checked, and either broken is a stretched picture.
void BindPipeline(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                  const Pipeline& pipeline, VkRect2D area) noexcept;

// The same with values from somewhere else. Separate rather than a defaulted argument,
// so a caller with nothing to override cannot name one by accident.
void BindPipeline(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                  const Pipeline& pipeline, VkRect2D area,
                  const RasterState& instead) noexcept;

// Output: writes all four channels, mixes nothing
constexpr VkPipelineColorBlendAttachmentState NoBlend() noexcept {
    VkPipelineColorBlendAttachmentState state{};
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                         | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    return state;
}

// Output: straight alpha, src*a + dst*(1-a), with the alpha channel left at src
//
// An sRGB attachment decodes, blends in linear space and encodes again, so the result is
// not a halfway mix of the stored bytes -- and that is the correct one.
constexpr VkPipelineColorBlendAttachmentState AlphaBlend() noexcept {
    VkPipelineColorBlendAttachmentState state = NoBlend();
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    return state;
}

// Everything one pipeline is created from: every field of VkGraphicsPipelineCreateInfo
// that is ours to choose, and there are two answers rather than three.
//
//   here        it can differ between pipelines
//   derived     it comes from another field here, so a second copy could disagree --
//               the sample count and attachment formats from targets, the winding from
//               viewportY, the stencil format from whether the depth one has the aspect
//
// A field is not left out because a feature is missing or because nothing sets it yet.
// Saying what you want belongs here; saying whether it is possible belongs to
// CreateGraphicsPipeline, which refuses a value the device was never asked for and
// names the feature. Left out instead, the answer would be "edit Pipeline.cpp", and
// this layer would be deciding what its callers may ask.
struct GraphicsPipelineDesc {

    // Borrowed. Two pipelines differing only in baked state share one.
    const ShaderProgram* program = nullptr;

    // stride 0 is no vertex buffer -- the shader builds its points from gl_VertexIndex.
    VertexLayout vertexLayout;

    // Compiled in. VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY moves only inside a class without
    // extendedDynamicState3 -- list to strip, not triangles to lines.
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // The index that starts a new strip or fan instead of adding to one.
    // Needs primitiveTopologyListRestart on a list topology.
    VkBool32 primitiveRestart = VK_FALSE;

    // The contract it is compiled against, and not the images that will satisfy it.
    //
    // VkPipelineRenderingCreateInfo takes colour formats, a depth format and a stencil
    // one; the sample count comes from the multisample state. A resource is named
    // nowhere in either, so naming one here claimed a dependency that does not exist --
    // and it was that claim which made a target list the only way to say what a
    // pipeline draws into, before any image the swapchain will hand over exists.
    //
    // AttachmentFormatsFor builds one out of descs with the roles said by position.
    AttachmentFormats formats;

    // LINE needs the fillModeNonSolid feature, which Core.h asks for and Device.cpp
    // refuses a GPU without.
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

    // Added to every fragment's depth before the test, in units of the format's
    // smallest representable difference plus a slope term. The tool for shadow acne,
    // and free of any feature -- an alternative to biasing inside the shader.
    VkBool32 depthBias = VK_FALSE;
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    float depthBiasClamp = 0.0f;      // 0 is no clamp

    // The fragment's alpha becomes a coverage mask, so a cutout gets the same edge
    // smoothing the rasterizer gives geometry. Only means anything at more than one
    // sample; the alternative is discard in the shader, which does not antialias.
    VkBool32 alphaToCoverage = VK_FALSE;

    // Which samples may be written at all, ANDed with coverage. One word covers up to
    // 32 samples, and kMaxSamples is far below that.
    VkSampleMask sampleMask = ~0u;

    // Shades every sample rather than every pixel, at minSampleShading of them at
    // least. Needs sampleRateShading.
    VkBool32 sampleShading = VK_FALSE;
    float minSampleShading = 0.0f;

    // Forces the written alpha to 1 after the shader. Needs alphaToOne.
    VkBool32 alphaToOne = VK_FALSE;

    // Keeps a fragment outside the near/far range instead of discarding it, which is
    // what a shadow caster behind the light's near plane needs. Needs depthClamp.
    VkBool32 depthClamp = VK_FALSE;

    // Wider than one pixel needs wideLines, and only LINE reads it.
    float lineWidth = 1.0f;

    // A second depth test against a fixed range, independent of depthCompare.
    // Needs depthBounds.
    VkBool32 depthBoundsTest = VK_FALSE;
    float minDepthBounds = 0.0f;
    float maxDepthBounds = 1.0f;

    // A bitwise operation between the fragment and the attachment, instead of blending
    // -- the two are exclusive. Needs logicOp.
    VkBool32 logicOpEnable = VK_FALSE;
    VkLogicOp logicOp = VK_LOGIC_OP_COPY;

    // Read only by a CONSTANT_* blend factor, and by every attachment that names one:
    // there is one set of these per pipeline, not per attachment.
    float blendConstants[4]{};

    // Which layers of a layered target this pipeline writes, as a bitmask.
    // Needs multiview.
    uint32_t viewMask = 0;

    // Enabling this needs a stencil attachment to read, and formats.stencil is where
    // the desc says whether there is one. CreateGraphicsPipeline refuses the pair --
    // a contract until the format was declared rather than inferred, because until
    // then the only thing to ask was the depth format, which is a different question.
    VkBool32 stencilTest = VK_FALSE;
    VkStencilOpState stencilFront{};
    VkStencilOpState stencilBack{};

    // One per colour target, in targets[] order. Vulkan's own type and one entry each,
    // because an attachment is where a blend applies and two are free to differ.
    //
    // Zeroed, so a colour target nothing was said about writes no channels --
    // CreateGraphicsPipeline refuses that rather than compiling a black attachment.
    //
    // Blending with depthWrite on hides what is drawn after, which is usually a
    // mistake and is not one Vulkan forbids -- additive over opaque geometry wants
    // exactly that. So this is not checked anywhere: a refusal would refuse a legal
    // picture. The one pipeline here that blends is the panel's, and it has the depth
    // test off, which is what makes depthWrite moot rather than agreed.
    VkPipelineColorBlendAttachmentState blend[kMaxColorTargets]{};

    // What every state is filled with -- baked in, or issued by BindPipeline if
    // dynamicStates names it. One value serves both answers.
    RasterState raster;

    // Which of them stay registers. The two at the front are not optional: a baked
    // viewport needs a VkViewport at creation and the extent is not known yet.
    //
    // Contract: a dynamic state must be set before every draw. BindPipeline issues
    //           exactly this list, which is why it is the same list.
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
    const VulkanDevice* dev = nullptr;        // non-owning, needed to destroy
    const ShaderProgram* program = nullptr;   // borrowed, shared between variants
    VkPipeline handle = VK_NULL_HANDLE;

    // What it was created with. Kept so callers stop carrying their own: a copy beside a
    // pipeline is a second value that can disagree with what was baked. Not the desc,
    // which points at TextureDescs whose extents move.
    VertexLayout vertexLayout;
    AttachmentFormats formats;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    VkPipelineColorBlendAttachmentState blend[kMaxColorTargets]{};
    RasterState raster;
    VkDynamicState dynamicStates[kMaxDynamicStates]{};
    uint32_t dynamicCount = 0;

    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};

// Effect: destroys the compiled object and leaves the struct empty. The layout and set
//         layouts are the program's, so a rebuild keeps every set already allocated.
//
// Contract: every command buffer using this pipeline must have finished, so the caller
//           calls vkDeviceWaitIdle - one frame's fence is not enough.
void DestroyPipeline(const VulkanDevice& dev, Pipeline* pipeline) noexcept;

// Effect: builds one pipeline from the desc, after checking both ends of the shader
//         boundary -- the layout against the vertex stage's inputs, the colour formats
//         against the fragment stage's outputs.
//
// Contract: formats must be what the attachments actually are. Nothing here can see the
//           images, so this one stays a contract.
bool CreateGraphicsPipeline(const VulkanDevice& dev,
                            const GraphicsPipelineDesc& desc,
                            Pipeline* out) noexcept;
