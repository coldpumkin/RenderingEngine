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

    // The deferred half. The three colours and the depth are what the geometry pass
    // draws into; sceneResolve is what the lighting pass draws into, and it is the
    // same image the scene pass resolves into -- which is what lets the post pass not
    // know which of the two ran.
    const TextureDesc* gAlbedo = nullptr;
    const TextureDesc* gNormal = nullptr;
    const TextureDesc* gMaterial = nullptr;
    const TextureDesc* gDepth = nullptr;
    const TextureDesc* sceneResolve = nullptr;

    // What a face of the sky cube is, which is the 2D slice the bake draws into.
    const TextureDesc* skyFace = nullptr;

    // What the caller holds these programs to, and the two halves answer different
    // questions.
    //
    // surfaceSets is what a set is made of, and only the programs that draw a surface
    // are held to it -- a program that reads no material would be refused for a set it
    // does not use.
    //
    // blocks is what one uniform block looks like inside, and **every program is held
    // to it**: a block is the same block wherever it is bound, and shadow.vert binds
    // one of these at a different slot than scene.frag does.
    const RequiredSet* surfaceSets = nullptr;
    uint32_t surfaceSetCount = 0;
    const RequiredBlock* blocks = nullptr;
    uint32_t blockCount = 0;
    const RequiredMember* pushMembers = nullptr;
    uint32_t pushMemberCount = 0;
};

// Six programs and eight pipelines. Two of the extra pipelines are wireframe variants
// -- polygonMode is compiled in, so a second value is a second pipeline, and sharing
// the program is what lets every set drawn from it fit both.
//
// **The forward and the deferred path are three programs each and share two of them.**
// shadow and post are neither path's: one draws the map both read and the other puts
// whichever result there is on the screen.
//
//   forward    shadow  scene   post  gui
//   deferred   shadow  geometry lighting  post  gui
//
// geometry and scene are held to the same MaterialSet(), which is what lets one set of
// material sets fit both -- and is the whole reason a second surface program was
// possible without a second copy of every material.
//
// Declared programs first so they are destroyed last -- every pipeline points at one,
// and every descriptor set was drawn from one of their layouts.
struct Pipelines {
    ShaderProgram shadowProgram;
    ShaderProgram sceneProgram;
    ShaderProgram geometryProgram;
    ShaderProgram lightingProgram;
    ShaderProgram skyBakeProgram;
    ShaderProgram skyProgram;
    ShaderProgram postProgram;
    ShaderProgram guiProgram;

    Pipeline shadow;
    Pipeline scene;
    Pipeline sceneWire;
    Pipeline geometry;
    Pipeline geometryWire;
    Pipeline lighting;

    // Three for one subject: the bake writes a cube face, and the two others draw the
    // sky into what each path's middle fills. Same program for the last two, and the
    // sample count is the whole of the difference.
    Pipeline skyBake;
    Pipeline skyForward;
    Pipeline skyDeferred;
    Pipeline post;
    Pipeline gui;
};

// Effect: loads every program and compiles every pipeline against sources.
// Output: false on the first one that fails, having logged which
bool CreatePipelines(const VulkanDevice& dev, const PipelineSources& sources,
                     Pipelines* out) noexcept;
