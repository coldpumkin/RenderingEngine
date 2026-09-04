#pragma once

#include "Vulkan/Attachments.h"
#include "Vulkan/Device.h"
#include "Vulkan/Shader.h"
#include "Vulkan/VertexLayout.h"

// ViewportY - one sign that decides two things, both of them at record time
// ============================================================================
//
// A negative viewport height makes the shader side y-up, and the same negation flips
// the winding test. So the sign chooses frontFace as well, and written by hand in two
// places the two disagree silently until culling is turned on.
//
// Neither half is baked. The viewport never was -- it is dynamic state so a resize
// rebuilds nothing -- and frontFace joined it: VK_DYNAMIC_STATE_FRONT_FACE is core in
// Vulkan 1.3, which we require, and the winding test is a register the same way cull
// mode is. That leaves the sign where it belongs, which is the pass: every draw in one
// shares a viewport, and nothing about it has to be known when a pipeline is compiled.
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
//
// area rather than an extent because the rect a pass draws into is not always the
// whole of what it draws on. Three of the four passes hand in the whole target; the
// post pass hands in a smaller one and the difference is the bars.
VkViewport MakeViewport(VkRect2D area, ViewportY y) noexcept;

// RasterState - what a pass settles before its first draw
// ============================================================================
//
// Every one of these is dynamic state, which means two things. It is not compiled in,
// so changing it costs a command rather than a pipeline. And **Vulkan remembers none
// of it across a command buffer**, so every pass has to set every one before it draws
// -- not only the ones it cares about.
//
// That second half is why this is a struct. Listed by hand, each pass names seven
// values and a new one means editing four passes; missed, the validation layer says
// so but only at run time. As a value, a pass names what it wants and the defaults
// answer for the rest.
//
// The defaults are what a pass that draws one flat thing wants: no culling, no depth,
// triangles. The scene pass overrides most of them and the shadow pass two.
//
// What is *not* here is the other half of the same question. polygonMode, blending,
// sample count and the attachment formats are compiled into a pipeline, so they live
// in GraphicsPipelineDesc and a second value means a second pipeline. The split is
// not how often something changes -- it is whether the driver has to compile
// something different.
//
// Primitive topology looked like it belonged here and does not. There is a
// VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, but without extendedDynamicState3 it only
// moves inside a class -- list to strip, not triangles to lines. Tried, and the
// validation layer said so at the first draw:
//
//   the last primitive topology POINT_LIST set by vkCmdSetPrimitiveTopology is not
//   compatible with the pipeline topology TRIANGLE_LIST
//
// So being in the dynamic-state enum is not the same as being free to change. Drawing
// this mesh as points would take a second pipeline, and a vertex shader that writes
// gl_PointSize, which the layer also asked for.
struct RasterState {
    // Decides the viewport's sign and the winding test together. Their pairing is the
    // reason it is one field: apart, a pass could set a viewport and inherit whatever
    // winding ran before it, which is invisible until something is culled.
    ViewportY viewportY = ViewportY::Down;

    // The starting value. The scene pass changes it per draw, from the material.
    VkCullModeFlags cull = VK_CULL_MODE_NONE;

    // Off means nothing is hidden and the draw order is what survives. depthWrite is
    // independent, but the spec makes it irrelevant while the test is off.
    VkBool32 depthTest = VK_FALSE;
    VkBool32 depthWrite = VK_FALSE;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;   // clear is 1.0, so nearer wins


    // Everything up to the rasterizer still runs; nothing after it does. What that
    // leaves is a pass that costs its vertex work and writes nothing.
    VkBool32 rasterizerDiscard = VK_FALSE;
};

// Effect: issues every dynamic state this program declares, in one call
//
// One call and not seven, so a pass cannot set some and inherit the rest. area is
// separate because it is the frame's rather than the pass's preference -- the same
// RasterState is right at any size.
//
// The viewport built from area is the one place in this program where a coordinate
// stops being a fraction and becomes a pixel. Two contracts meet on that line, they
// are separate, and nothing checks either -- either one broken is a stretched picture
// and neither says a word.
//
// Contract: 3D -> 2D. viewport.width / |viewport.height| equals the aspect the
//           projection behind these primitives was built with. The shadow pass holds
//           it by being square (its ortho box is), the scene pass by proj and this
//           area both reading kRenderExtent.
//
// Contract: 2D -> 2D. A pass that draws an image rather than geometry owes the same
//           thing with two extents -- what it samples against what it draws into --
//           and it is a separate question, because there is no projection here to
//           agree with. The post pass is the only one that owes it, and holds it by
//           handing in a letterboxed area (LetterboxInto in Passes.cpp) instead of
//           the whole target.
//
// area is why this takes a rect at all. Three passes pass {{0, 0}, extent} and always
// will; the fourth is the reason the offset exists.
void SetRasterState(const VolkDeviceTable& vk, VkCommandBuffer cmd,
                    VkRect2D area, const RasterState& raster) noexcept;

// Blending - whether the fragment is mixed with what is already there
// ============================================================================
//
// It used to carry depth write with it, on the grounds that a translucent surface
// writing depth hides what is drawn behind it later and one value cannot disagree
// with itself. Depth write is dynamic state now, so the two are apart again and
// keeping them in step is the recording side's -- a pass that binds a translucent
// pipeline is the one that has to leave depthWrite off.
//
// Not made dynamic itself: blending changes what the driver compiles into the
// fragment output stage, which is the line everything in GraphicsPipelineDesc is on.
enum class Blending {
    Opaque,
    Translucent,
};

// One pipeline's worth of decisions. Everything not here is the same in both of
// ours and lives in CreateGraphicsPipeline. The rows are where each value comes
// from, which is the whole reason this is a struct and not five arguments:
//
//   the mesh supplies     vertexLayout
//   the pass decides      formats
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

    // What it draws into, as the descriptions the images are made from -- one type
    // says what a target is, and this points at it rather than restating any of it.
    //
    // Pointers, and read during creation only: Pipeline keeps the projection instead.
    // A resize remakes these descs at a new extent (ResizeScenePass) and rebuilds no
    // pipeline, so what is kept has to be the part that does not move.
    //
    // The first null ends the colour list, so how many there are is the list itself.
    // An empty one is a depth-only pass; depth null is a pass with no depth. The
    // fragment stage has the final say on the count and is checked against it.
    const TextureDesc* color[kMaxColorTargets]{};
    const TextureDesc* depth = nullptr;


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

    // What it was compiled against, and the whole of what a later check can ask about
    // it. Not the desc it came from: that points at TextureDescs whose extents move.
    //
    // Keeping them here is what lets callers stop carrying their own -- a copy beside
    // a pipeline is a second value that can disagree with what was baked, and after
    // the fact there is nothing to check it against.
    VertexLayout vertexLayout;
    AttachmentFormats formats;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    Blending blending = Blending::Opaque;

    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};

// Effect: destroys the compiled object and leaves the struct empty. The layout and
//         set layouts are the program's and are not touched, so a rebuild would keep
//         every set already allocated. Only the destructor calls this today.
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
