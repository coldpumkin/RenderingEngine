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
    // Depth and no colour, which is what a program with no fragment stage means -- so
    // blend[] stays empty. The null says it: there is no colour attachment to name.
    desc.formats = AttachmentFormatsFor(nullptr, 0, sources.shadowDepth);

    // ViewportY::Down settles the direction the map's v axis runs; a reader turning ndc
    // back into a uv has to use this sign. The winding rides along and has no effect
    // while nothing is culled.
    desc.raster.viewportY = ViewportY::Down;
    desc.raster.depthTest = VK_TRUE;
    desc.raster.depthWrite = VK_TRUE;
    return desc;
}

GraphicsPipelineDesc PointShadowDesc(const ShaderProgram& program,
                                    const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    desc.vertexLayout = sources.meshLayout;

    // Colour and depth both, unlike the 2D shadow pipeline: what this pass stores is a
    // distance the fragment stage computes, so it has a fragment stage and an
    // attachment to write.
    const TextureDesc* const colour[] = {sources.pointShadowFace};
    desc.formats = AttachmentFormatsFor(colour, 1, sources.pointShadowDepth);
    desc.blend[0] = NoBlend();

    // Down, because the six face views are built in the cube convention, which already
    // has y running the other way. Up here would flip every face a second time.
    desc.raster.viewportY = ViewportY::Down;
    desc.raster.depthTest = VK_TRUE;
    desc.raster.depthWrite = VK_TRUE;

    // Nothing culled. Which way a triangle winds in a face's clip space depends on the
    // face, and a shadow caster seen from inside still occludes.
    desc.raster.cull = VK_CULL_MODE_NONE;
    return desc;
}

GraphicsPipelineDesc BloomDesc(const ShaderProgram& program,
                              const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;

    // No vertex buffer: fullscreen.vert builds its three points from gl_VertexIndex.
    const TextureDesc* const colour[] = {sources.bloomTarget};
    desc.formats = AttachmentFormatsFor(colour, 1, nullptr);
    desc.blend[0] = NoBlend();
    return desc;
}

GraphicsPipelineDesc SceneDesc(const ShaderProgram& program,
                               const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    desc.vertexLayout = sources.meshLayout;
    // The multisample colour and the depth, not the resolve: what a pipeline is
    // compiled against is what it draws into, and the resolve is what leaves afterwards.
    const TextureDesc* const colour[] = {sources.sceneColor};
    desc.formats = AttachmentFormatsFor(colour, 1, sources.sceneDepth);
    desc.blend[0] = NoBlend();

    // Up, because a y-up world's projection was built that way; the winding rides along
    // in the same field.
    //
    // The one raster field this pipeline owns. The rest of the state this pass draws
    // with belongs to the panel, which writes every one of them at record time -- so
    // naming a depth or cull value here would be writing a value nothing reads.
    desc.raster.viewportY = ViewportY::Up;
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

// The same vertex stage and the same targets shape as the scene's, and three colour
// attachments where that has one. No blend enabled on any of them: a g-buffer records
// what is nearest, and mixing two surfaces' normals would make a direction that is
// neither. That is also why the deferred path has no answer for a transparent surface.
GraphicsPipelineDesc GeometryDesc(const ShaderProgram& program,
                                  const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    desc.vertexLayout = sources.meshLayout;

    // In geometry.frag's output order, and CheckOutputInterface holds the pipeline to
    // declaring exactly as many as the shader writes.
    const TextureDesc* const colour[] = {sources.gAlbedo, sources.gNormal,
                                        sources.gMaterial};
    desc.formats = AttachmentFormatsFor(colour, 3, sources.gDepth);
    desc.blend[0] = NoBlend();
    desc.blend[1] = NoBlend();
    desc.blend[2] = NoBlend();

    // Up, for the reason the scene's is: the same scene.vert, the same y-up world.
    desc.raster.viewportY = ViewportY::Up;
    return desc;
}

GraphicsPipelineDesc GeometryWireDesc(const ShaderProgram& program,
                                      const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc = GeometryDesc(program, sources);
    desc.polygonMode = VK_POLYGON_MODE_LINE;
    return desc;
}

// One colour and no depth, into the image the scene pass resolves into. The depth it
// needs is an input here rather than an attachment -- which is what the whole pass is.
GraphicsPipelineDesc LightingDesc(const ShaderProgram& program,
                                  const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    const TextureDesc* const colour[] = {sources.sceneResolve};
    desc.formats = AttachmentFormatsFor(colour, 1, nullptr);
    desc.blend[0] = NoBlend();

    // No vertex layout and cull BACK, the same two answers the post pipeline gives for
    // the same reason: this shares fullscreen.vert, whose one triangle is wound to
    // face us and whose uv expects the default orientation.
    desc.raster.cull = VK_CULL_MODE_BACK_BIT;
    return desc;
}

// One face of the sky cube. No blending, no depth, no culling -- one triangle covering
// a face, and nothing behind it to combine with.
GraphicsPipelineDesc SkyBakeDesc(const ShaderProgram& program,
                                 const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    const TextureDesc* const colour[] = {sources.skyFace};
    desc.formats = AttachmentFormatsFor(colour, 1, nullptr);
    desc.blend[0] = NoBlend();
    return desc;
}

// The sky into whatever the middle of a path fills. Two of these, and target is the one
// argument that differs -- the sample count comes with it.
GraphicsPipelineDesc SkyDesc(const ShaderProgram& program,
                             const TextureDesc* target) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    const TextureDesc* const colour[] = {target};
    desc.formats = AttachmentFormatsFor(colour, 1, nullptr);
    desc.blend[0] = NoBlend();
    return desc;
}

// A face of the irradiance cube. Same shape as the bake above and a different target:
// the size differs and the format does not, because it holds the same kind of quantity.
GraphicsPipelineDesc IrradianceDesc(const ShaderProgram& program,
                                    const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    const TextureDesc* const colour[] = {sources.irradianceFace};
    desc.formats = AttachmentFormatsFor(colour, 1, nullptr);
    desc.blend[0] = NoBlend();
    return desc;
}

// A face of one level of the prefiltered cube, and the BRDF table. Both are one
// triangle into one colour target; what differs is the format, which comes with it.
GraphicsPipelineDesc PrefilterDesc(const ShaderProgram& program,
                                   const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    const TextureDesc* const colour[] = {sources.prefilterFace};
    desc.formats = AttachmentFormatsFor(colour, 1, nullptr);
    desc.blend[0] = NoBlend();
    return desc;
}

GraphicsPipelineDesc BrdfLutDesc(const ShaderProgram& program,
                                 const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    const TextureDesc* const colour[] = {sources.brdfLut};
    desc.formats = AttachmentFormatsFor(colour, 1, nullptr);
    desc.blend[0] = NoBlend();
    return desc;
}

GraphicsPipelineDesc PostDesc(const ShaderProgram& program,
                              const PipelineSources& sources) noexcept {
    GraphicsPipelineDesc desc;
    desc.program = &program;
    const TextureDesc* const colour[] = {sources.swapchain};
    desc.formats = AttachmentFormatsFor(colour, 1, nullptr);
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
    const TextureDesc* const colour[] = {sources.swapchain};
    desc.formats = AttachmentFormatsFor(colour, 1, nullptr);
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

    // Two stages, where the 2D shadow has one: a cube stores a distance rather than a
    // depth, and a distance is something a fragment stage has to work out.
    const char* const pointShadowStages[] = {"Shaders/pointshadow.vert.spv",
                                             "Shaders/pointshadow.frag.spv"};
    const char* const sceneStages[] = {"Shaders/scene.vert.spv", "Shaders/scene.frag.spv"};

    // **scene.vert again, unchanged.** Where a surface sits does not depend on when it
    // is shaded, so the deferred path needed no vertex stage of its own -- which is
    // most of why it is two shaders and not four.
    const char* const geometryStages[] = {"Shaders/scene.vert.spv",
                                          "Shaders/geometry.frag.spv"};
    // fullscreen.vert keeps its name because it is the half that is not post's: this
    // is the second pipeline pairing the same module with a different fragment stage,
    // which is what that name was written for.
    const char* const lightingStages[] = {"Shaders/fullscreen.vert.spv",
                                          "Shaders/lighting.frag.spv"};
    const char* const skyBakeStages[] = {"Shaders/fullscreen.vert.spv",
                                         "Shaders/skybake.frag.spv"};
    const char* const irradianceStages[] = {"Shaders/fullscreen.vert.spv",
                                            "Shaders/irradiance.frag.spv"};
    const char* const prefilterStages[] = {"Shaders/fullscreen.vert.spv",
                                           "Shaders/prefilter.frag.spv"};
    const char* const brdfLutStages[] = {"Shaders/fullscreen.vert.spv",
                                         "Shaders/brdflut.frag.spv"};
    const char* const skyStages[] = {"Shaders/fullscreen.vert.spv", "Shaders/sky.frag.spv"};
    const char* const bloomExtractStages[] = {"Shaders/fullscreen.vert.spv",
                                              "Shaders/bloom_extract.frag.spv"};
    const char* const bloomBlurStages[] = {"Shaders/fullscreen.vert.spv",
                                           "Shaders/bloom_blur.frag.spv"};
    const char* const postStages[] = {"Shaders/fullscreen.vert.spv", "Shaders/post.frag.spv"};
    const char* const guiStages[] = {"Shaders/gui.vert.spv", "Shaders/gui.frag.spv"};

    // Only the programs that draw a surface are held to the shared sets, and there are
    // two of them now: scene and geometry. That is what MaterialSet() was for -- one
    // set of material sets fits both, and a mismatch is refused here rather than
    // showing up as four samplers in the wrong order.
    //
    // shadow writes depth, lighting reads images, post copies one and gui draws a
    // panel. None of them reads a material, and requiring one of them to would be
    // requiring a set they do not use -- lighting's set 1 is its g-buffer.
    // Three requirements, each adding to the one before it.
    //
    // **any** is the shared uniform blocks, and every program is held to them: a block
    // is the same block wherever it is bound, and shadow.vert binds one at a different
    // slot than scene.frag does.
    //
    // **mesh** adds the per-draw push block. gui has a push block of its own -- window
    // pixels to clip space, nothing to do with a DrawItem -- so it is not in this
    // group, and lighting and post push nothing at all.
    //
    // **surface** adds the material set, for the two that read one.
    ProgramRequirements anyProgram;
    anyProgram.blocks = sources.blocks;
    anyProgram.blockCount = sources.blockCount;

    ProgramRequirements meshProgram = anyProgram;
    meshProgram.pushMembers = sources.pushMembers;
    meshProgram.pushMemberCount = sources.pushMemberCount;

    ProgramRequirements surfaceProgram = meshProgram;
    surfaceProgram.sets = sources.surfaceSets;
    surfaceProgram.setCount = sources.surfaceSetCount;

    if (!CreateShaderProgram(dev, shadowStages,
                             static_cast<uint32_t>(std::size(shadowStages)),
                             meshProgram, &out->shadowProgram)
            || !CreateShaderProgram(dev, pointShadowStages,
                                    static_cast<uint32_t>(std::size(pointShadowStages)),
                                    meshProgram, &out->pointShadowProgram)
            || !CreateShaderProgram(dev, sceneStages,
                                    static_cast<uint32_t>(std::size(sceneStages)),
                                    surfaceProgram, &out->sceneProgram)
            || !CreateShaderProgram(dev, geometryStages,
                                    static_cast<uint32_t>(std::size(geometryStages)),
                                    surfaceProgram, &out->geometryProgram)
            || !CreateShaderProgram(dev, lightingStages,
                                    static_cast<uint32_t>(std::size(lightingStages)),
                                    anyProgram, &out->lightingProgram)
            || !CreateShaderProgram(dev, skyBakeStages,
                                    static_cast<uint32_t>(std::size(skyBakeStages)),
                                    anyProgram, &out->skyBakeProgram)
            || !CreateShaderProgram(dev, irradianceStages,
                                    static_cast<uint32_t>(std::size(irradianceStages)),
                                    anyProgram, &out->irradianceProgram)
            || !CreateShaderProgram(dev, prefilterStages,
                                    static_cast<uint32_t>(std::size(prefilterStages)),
                                    anyProgram, &out->prefilterProgram)
            || !CreateShaderProgram(dev, brdfLutStages,
                                    static_cast<uint32_t>(std::size(brdfLutStages)),
                                    anyProgram, &out->brdfLutProgram)
            || !CreateShaderProgram(dev, skyStages,
                                    static_cast<uint32_t>(std::size(skyStages)),
                                    anyProgram, &out->skyProgram)
            || !CreateShaderProgram(dev, bloomExtractStages,
                                    static_cast<uint32_t>(std::size(bloomExtractStages)),
                                    anyProgram, &out->bloomExtractProgram)
            || !CreateShaderProgram(dev, bloomBlurStages,
                                    static_cast<uint32_t>(std::size(bloomBlurStages)),
                                    anyProgram, &out->bloomBlurProgram)
            || !CreateShaderProgram(dev, postStages,
                                    static_cast<uint32_t>(std::size(postStages)),
                                    anyProgram, &out->postProgram)
            || !CreateShaderProgram(dev, guiStages,
                                    static_cast<uint32_t>(std::size(guiStages)),
                                    anyProgram, &out->guiProgram)) {
        return false;
    }

    return CreateGraphicsPipeline(dev, ShadowDesc(out->shadowProgram, sources),
                                  &out->shadow)
        && CreateGraphicsPipeline(dev, PointShadowDesc(out->pointShadowProgram, sources),
                                  &out->pointShadow)
        && CreateGraphicsPipeline(dev, SceneDesc(out->sceneProgram, sources),
                                  &out->scene)
        && CreateGraphicsPipeline(dev, SceneWireDesc(out->sceneProgram, sources),
                                  &out->sceneWire)
        && CreateGraphicsPipeline(dev, GeometryDesc(out->geometryProgram, sources),
                                  &out->geometry)
        && CreateGraphicsPipeline(dev, GeometryWireDesc(out->geometryProgram, sources),
                                  &out->geometryWire)
        && CreateGraphicsPipeline(dev, LightingDesc(out->lightingProgram, sources),
                                  &out->lighting)
        && CreateGraphicsPipeline(dev, SkyBakeDesc(out->skyBakeProgram, sources),
                                  &out->skyBake)
        && CreateGraphicsPipeline(dev, IrradianceDesc(out->irradianceProgram, sources),
                                  &out->irradianceBake)
        && CreateGraphicsPipeline(dev, PrefilterDesc(out->prefilterProgram, sources),
                                  &out->prefilterBake)
        && CreateGraphicsPipeline(dev, BrdfLutDesc(out->brdfLutProgram, sources),
                                  &out->brdfLutBake)
        && CreateGraphicsPipeline(dev, SkyDesc(out->skyProgram, sources.sceneColor),
                                  &out->skyForward)
        && CreateGraphicsPipeline(dev, SkyDesc(out->skyProgram, sources.sceneResolve),
                                  &out->skyDeferred)
        && CreateGraphicsPipeline(dev, BloomDesc(out->bloomExtractProgram, sources),
                                  &out->bloomExtract)
        && CreateGraphicsPipeline(dev, BloomDesc(out->bloomBlurProgram, sources),
                                  &out->bloomBlur)
        && CreateGraphicsPipeline(dev, PostDesc(out->postProgram, sources),
                                  &out->post)
        && CreateGraphicsPipeline(dev, GuiDesc(out->guiProgram, sources),
                                  &out->gui);
}
