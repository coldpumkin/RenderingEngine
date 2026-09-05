#include "Pipelines.h"

#include <iterator>   // std::size

namespace {

// One helper per pipeline, and none of them public: nobody outside this file builds a
// GraphicsPipelineDesc, so what a pipeline is made of is settled in one place.

GraphicsPipelineDesc ShadowDesc(const ShaderProgram& program,
                                const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    desc.vertexLayout = sources.meshLayout;
    // One target, and its usage says it is the depth one. No colour follows, which is
    // what a program with no fragment stage means -- so blend[] stays empty.
    desc.targets[0] = sources.shadowDepth;

    // ViewportY::Down settles the direction the map's v axis runs; a reader turning ndc
    // back into a uv has to use this sign. The winding rides along and has no effect
    // while nothing is culled.
    desc.raster.viewportY = ViewportY::Down;
    desc.raster.depthTest = VK_TRUE;
    desc.raster.depthWrite = VK_TRUE;
    return desc;
}

GraphicsPipelineDesc SceneDesc(const ShaderProgram& program,
                               const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    desc.vertexLayout = sources.meshLayout;
    // The multisample colour and the depth, not the resolve: a pipeline bakes what it
    // draws into, and the resolve is what leaves afterwards.
    desc.targets[0] = sources.sceneColor;
    desc.targets[1] = sources.sceneDepth;
    desc.blend[0] = NoBlend();

    // Up, because a y-up world's projection was built that way; the winding rides along
    // in the same field. cull starts at NONE and a draw changes it per material, so
    // what is here is the value a draw inherits before its material speaks.
    desc.raster.viewportY = ViewportY::Up;
    desc.raster.depthTest = VK_TRUE;
    desc.raster.depthWrite = VK_TRUE;
    return desc;
}

// Built from the one above rather than beside it, because one changed field is the
// whole of what a second variant is. LINE needs fillModeNonSolid, which Core.h asks
// for and CreateGraphicsPipeline checks.
GraphicsPipelineDesc SceneWireDesc(const ShaderProgram& program,
                                   const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc = SceneDesc(program, sources);
    desc.polygonMode = VK_POLYGON_MODE_LINE;
    return desc;
}

GraphicsPipelineDesc PostDesc(const ShaderProgram& program,
                              const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    desc.targets[0] = sources.swapchain;
    desc.blend[0] = NoBlend();

    // No vertex layout: fullscreen.vert builds its three points from gl_VertexIndex,
    // and a shader that makes its own vertices needs no buffer described. Down, because
    // that shader's uv expects the default orientation; one triangle wound to face us,
    // and nothing to hide behind anything.
    desc.raster.cull = VK_CULL_MODE_BACK_BIT;
    return desc;
}

GraphicsPipelineDesc GuiDesc(const ShaderProgram& program,
                             const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    desc.vertexLayout = sources.guiLayout;
    desc.targets[0] = sources.swapchain;
    // The only one of the five that blends: a panel has to be see-through to be over
    // anything. No depth -- it draws last, on top.
    desc.blend[0] = AlphaBlend();
    return desc;
}

}   // namespace

bool CreatePipelines(const VulkanDevice& dev, const PipelineSources& sources,
                     Pipelines* out) noexcept {
    // One stage. A shadow pipeline writes depth and nothing else, and depth comes from
    // the fixed-function test out of gl_Position -- so there is no fragment stage.
    const char* const shadowStages[] = {"Shaders/shadow.vert.spv"};
    const char* const sceneStages[] = {"Shaders/scene.vert.spv", "Shaders/scene.frag.spv"};
    // fullscreen.vert keeps its name because it is the half that is not post's: a
    // lighting pipeline would pair the same module with a different fragment stage.
    const char* const postStages[] = {"Shaders/fullscreen.vert.spv", "Shaders/post.frag.spv"};
    const char* const guiStages[] = {"Shaders/gui.vert.spv", "Shaders/gui.frag.spv"};

    // Only the programs that draw a surface are held to the shared sets. shadow writes
    // depth, post copies an image and gui draws a panel -- none of them reads a
    // material, and requiring one of them to would be requiring a set they do not use.
    if (!CreateShaderProgram(dev, shadowStages,
                             static_cast<uint32_t>(std::size(shadowStages)),
                             nullptr, 0, &out->shadowProgram)
            || !CreateShaderProgram(dev, sceneStages,
                                    static_cast<uint32_t>(std::size(sceneStages)),
                                    sources.required, sources.requiredCount,
                                    &out->sceneProgram)
            || !CreateShaderProgram(dev, postStages,
                                    static_cast<uint32_t>(std::size(postStages)),
                                    nullptr, 0, &out->postProgram)
            || !CreateShaderProgram(dev, guiStages,
                                    static_cast<uint32_t>(std::size(guiStages)),
                                    nullptr, 0, &out->guiProgram)) {
        return false;
    }

    return CreateGraphicsPipeline(dev, ShadowDesc(out->shadowProgram, sources),
                                  &out->shadow)
        && CreateGraphicsPipeline(dev, SceneDesc(out->sceneProgram, sources),
                                  &out->scene)
        && CreateGraphicsPipeline(dev, SceneWireDesc(out->sceneProgram, sources),
                                  &out->sceneWire)
        && CreateGraphicsPipeline(dev, PostDesc(out->postProgram, sources),
                                  &out->post)
        && CreateGraphicsPipeline(dev, GuiDesc(out->guiProgram, sources),
                                  &out->gui);
}
