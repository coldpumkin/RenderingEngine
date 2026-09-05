#pragma once

#include "Vulkan/Pipeline.h"

// Pipelines - how each piece of work runs on the GPU
// ============================================================================
//
// The other half of a pass. A pass says what work is being done -- what it draws into,
// what it loads and stores, what it reads. This says how that work executes: which
// shaders, what the vertices look like, what the rasterizer and the blend do.
//
// The two share exactly one thing, and neither owns the other: a TextureDesc. The pass
// draws into it; a pipeline is compiled against its format. So this file includes no
// pass header, and a pass holds no pipeline definition.
//
// Every shader path this program loads is here, because which shaders run is part of
// how the work executes and not part of what the work is.

// What the pipelines are compiled against. Pointers for the targets, because a resize
// remakes those descs at a new extent and rebuilds no pipeline.
//
// Contract: everything pointed at must outlive CreatePipelines. The descs are read
//           during creation and the projections are what a Pipeline keeps.
struct PipelineSources {
    // One buffer, read by two vertex stages: the scene's takes every attribute, the
    // shadow's takes position. A layout describes the buffer, not the shader.
    VertexLayout meshLayout;
    VertexLayout guiLayout;

    const TextureDesc* shadowDepth = nullptr;
    const TextureDesc* sceneColor = nullptr;   // the multisample one, not the resolve
    const TextureDesc* sceneDepth = nullptr;
    const TextureDesc* swapchain = nullptr;
};

// Four programs and five pipelines. The extra one is the scene's wireframe variant:
// polygonMode is compiled in, so a second value is a second pipeline, and sharing the
// program is what lets every set drawn from it fit both.
//
// Declared programs first so they are destroyed last -- every pipeline points at one,
// and every descriptor set was drawn from one of their layouts.
struct Pipelines {
    ShaderProgram shadowProgram;
    ShaderProgram sceneProgram;
    ShaderProgram postProgram;
    ShaderProgram guiProgram;

    Pipeline shadow;
    Pipeline scene;
    Pipeline sceneWire;
    Pipeline post;
    Pipeline gui;
};

// Effect: loads every program and compiles every pipeline against sources.
// Output: false on the first one that fails, having logged which
bool CreatePipelines(const VulkanDevice& dev, const PipelineSources& sources,
                     Pipelines* out) noexcept;
