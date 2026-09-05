#pragma once

// ScenePass - the off-screen pass, and the surfaces it draws
// ============================================================================
//
//   what              where it comes from      who else uses it
//   its three images  main makes them          the post pass samples the resolve,
//                                              and so does the capture
//   the shadow map    main makes it            the shadow pass drew it
//   camera, light,    main writes them         the shadow pass reads the same
//     shadow matrix     every frame              FrameShadow
//   the panel's       the gui pass owns it     asked for by name -- the one edge
//     switches                                   that runs backwards
//   its two sets      this pass makes them     nobody
//
// Owns nothing it draws with or into. What it owns is the material sets, which are
// what a surface looks like rather than where a frame goes.

#include "Gui.h"     // kCullFromMaterial, and the switches this pass reads
#include "Passes.h"

// The material's numbers, as the shader reads them. One per material, in set 1
// beside its images.
//
// Here rather than in PushConstants because it is counted by materials and that block
// is counted by draws -- one block, one rate, and the faster of the two wins.
//
// Contract: field order and types match the shader's MaterialBlock. std140 rounds a
//           block up to 16 bytes, so the leftover is named rather than hidden.
struct MaterialParams {
    glm::vec4 baseColorFactor{1.0f};   // rgb multiplies the texture, a its alpha

    float alphaCutoff = 0.0f;          // 0 keeps every texel

    // Both multiply the texture the way baseColorFactor does, and glTF defaults both
    // to 1: fully metallic and fully rough, which is what a material naming neither a
    // texture nor a factor asks for.
    float metallic = 1.0f;
    float roughness = 1.0f;

    float pad{};   // std140 rounds the block to 32
};


// Material - what a surface looks like, apart from where it is
// ============================================================================
//
// One set, drawn from the pool and filled once. The texture is not owned here: how
// many textures a scene has is the scene's business, and two materials naming the
// same image is normal.
//
// Four bindings, in the one set. The prediction written here has held three times now:
// each new thing a material owns is another binding, not another set, because they are
// all counted the same way -- one per material.
//
// The set is not all of it. What a surface looks like also decides one thing no shader
// can be handed, and that is the line the last field is on.
struct Material {
    VkDescriptorSet set = VK_NULL_HANDLE;

    // Binding 2 of that set. Owned rather than borrowed, unlike the textures: an image
    // can be shared between materials, these numbers are one per material by
    // definition.
    Buffer params;

    // glTF doubleSided, as the value the API wants. Here rather than on the draw
    // because it is counted the way the set is -- one per material, never per draw --
    // and a draw storing it again lets the two disagree.
    //
    // Not a binding: the rasterizer is not a shader input, so this is the one material
    // value that cannot ride in the set. It goes out as vkCmdSetCullMode instead.
    //
    // A function of the material only because the loader keys materials on it. Two
    // glTF materials sharing textures but differing here stay two.
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
};

// What one material is made of, before it becomes a Material. Pointers: the scene owns
// the textures, and two materials naming one image share it.
//
// Neither texture may be null. A material the asset left without a normal map takes a
// flat one, which is the caller's to supply -- this layer has no way to make a texture.
struct MaterialDesc {
    const Texture* baseColor = nullptr;
    const Texture* normal = nullptr;

    // glTF packs two values into one image: green is roughness, blue is metallic.
    // Red is free and some tools put occlusion there, which we do not read.
    const Texture* metallicRoughness = nullptr;

    MaterialParams params;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
};

// Effect: draws one set per material and points each at its textures
//
// Contract: pipeline must be the one these will be bound with -- the set is drawn
//           from its material layout.
bool CreateMaterials(const VulkanDevice& dev,
                     const Descriptors& descriptors, const DescriptorLayout& layout,
                     const MaterialDesc* sources, uint32_t count,
                     Material* out) noexcept;

// What a pass draws into, described the way every other texture here is
// ============================================================================
//
// A TextureDesc says four things and AttachmentFormats says two of them, so these are
// what a target actually is and the pipeline's value is derived from them. The two
// AttachmentFormats drops are the two that mattered all along:
//
//   extent   the aspect a projection is built with comes from here
//   usage    ATTACHMENT is what the pipeline draws into. **SAMPLED is an edge** --
//            it marks the images another pass reads, and there are exactly two
//
// Written by the caller, beside the other things it hands a pass, rather than made up
// inside pass creation from formats read back off a pipeline.
struct SceneTargetDescs {
    TextureDesc color;     // multisample. Drawn into, then discarded
    TextureDesc resolve;   // 1 sample. What leaves the pass
    TextureDesc depth;     // multisample. Never read outside the frame
};

// Output: the three, from one size and the formats
//
// The expansion rule, which used to be four lines inside CreateScenePass and a
// sentence in its comment. Two callers now -- creation and every resize.
SceneTargetDescs MakeSceneTargets(VkExtent2D extent, VkFormat colour, VkFormat depth,
                                  VkSampleCountFlagBits samples) noexcept;

// The three images those descs describe, made together and remade together
//
// Field for field with SceneTargetDescs, because that is what it is the product of.
// Owned by whoever declares one -- main does -- and the scene pass borrows it, the
// way both passes borrow the shadow map.
struct SceneTargets {
    Texture color;     // multisample. Drawn into, then discarded
    Texture resolve;   // 1 sample. vkCmdEndRendering averages into it, and the post
                       // pass samples it -- the one that leaves
    Texture depth;     // multisample. Tested and written, never read outside the frame
};

// Output: what the scene pipeline is compiled from
//
// The multisample colour and the depth, and not the resolve: a pipeline bakes what it
// draws into, and the resolve is what leaves afterwards. One colour output, which is
// what scene.frag declares and CheckOutputInterface compares this against.
//
// Contract: targets must outlive CreateGraphicsPipeline. The desc points into it.
GraphicsPipelineDesc MakeScenePipeline(const VertexLayout& mesh,
                                       const SceneTargetDescs& targets) noexcept;

// Output: the same pipeline with polygonMode LINE
//
// Built from the one above rather than beside it, because one changed field is the
// whole of what a second variant is -- and because sharing a ShaderProgram is what
// lets every set drawn from it fit both. LINE needs fillModeNonSolid, requested in
// Core.h.
GraphicsPipelineDesc MakeSceneWirePipeline(const VertexLayout& mesh,
                                           const SceneTargetDescs& targets) noexcept;

// Effect: makes the three, or remakes them at a new size
//
// Remaking releases first, view before image inside each -- see ResetTexture. The
// caller waits for the GPU: a frame in flight is still reading last frame's, and no
// fence here says which. The post pass's sets name the resolve, so they have to be
// refreshed afterwards; nothing else does.
bool CreateSceneTargets(const VulkanDevice& dev, const SceneTargetDescs& descs,
                        SceneTargets* out) noexcept;
bool ResizeSceneTargets(const VulkanDevice& dev, const SceneTargetDescs& descs,
                        SceneTargets* out) noexcept;

// ScenePass - the off-screen pass, and what it draws into
// ============================================================================
//
// The pass is one; its attachments are one set per frame in flight. Every frame
// draws into them again, so a frame cannot share them with one the GPU has not
// finished -- the first barrier in recording is srcStage TOP_OF_PIPE, which waits
// for nothing.
//
// The mesh sits beside frames[], not inside it: nothing writes it after creation, so
// every frame reads the same one. A pointer because the scene owns it.
//
// No texture here any more. It was one because there was one, and the moment a second
// arrived it stopped being a property of the pass -- it is a Material now, and the
// DrawItem says which.
//
// What the pass owns, and what reaches it from outside
// ----------------------------------------------------------------------------
//
// Sorted by that question rather than by type, because the answer is lopsided:
//
//   owns       the images it draws into. That is the whole list.
//
//   receives   the light                  an argument, one buffer per frame
//              the shadow maps            an argument, one image per frame
//              the raster switches        an argument to RecordScenePass
//              the camera                 main assigns into frames[i] from outside
//              the panel's buffer         asked for: GuiOptionsBuffer(gui, i)
//
// Three of the five now say what they are in the signature. The camera does not --
// main reaches in and writes it -- and the panel is deliberately different: it is a
// tool for making features comparable while they are understood, not a dependency of
// the same kind, and asking a Gui for its buffer is not the same as reaching into a
// pass for an image.
//
// This is written as a list and not as a type on purpose. Naming what a pass owns is
// what has to happen before anything derives from it, and a struct now would fix the
// answer while three of the four routes still have no reason to be what they are.
//
// The line is also not a partition. "The pass owns this" says nothing about what a
// draw owns, and the tempting reading -- everything else is the draw's -- would settle
// a question the asset is currently answering. The pipeline below is the case: it is
// the pass's because no material in Sponza asks for a second one, not because a pass
// is the thing that holds a pipeline.
struct ScenePass {
    const Mesh* mesh = nullptr;

    // The shader interface every draw in this pass answers to: set layouts, push
    // range, pipeline layout. It is the pass's and not a pipeline's, because a pass
    // may hold several pipelines and they all bind through this one.
    //
    // What Vulkan requires of a draw here is only that its pipeline was compiled for
    // these attachment formats. Sharing a program is our restriction, not the API's:
    // recording binds set 0 and pushes constants through this one layout, so a
    // pipeline from a different program would have to bring its own -- and every draw
    // would then carry which layout to use.
    //
    // That is the shape a second program in one pass would take, and it is why
    // "one program per pass" is written here rather than assumed: the day a draw needs
    // a different set layout against the same attachments, this field becomes the
    // draw's rather than the pass's.
    const ShaderProgram* program = nullptr;

    // Two variants of the one program, and the pass picks between them at record
    // time. They differ in polygonMode and in nothing else -- same shaders, same set
    // layouts, same push range, same attachment formats.
    //
    // That sameness is the point rather than a coincidence: every descriptor set this
    // pass allocated was drawn from program's layouts, so switching between these two
    // rebinds nothing. It is what "a pipeline is one variant of a program" means when
    // there is finally more than one.
    //
    // Not on the DrawItem. Which pipeline is used is one answer for the whole pass,
    // not something a draw decides -- and this asset gives no reason for it to be:
    // 25 materials, 22 OPAQUE and 3 MASK, and MASK is a discard in the shader.
    const Pipeline* pipeline = nullptr;
    const Pipeline* wirePipeline = nullptr;

    struct PerFrame {
        // **Borrowed.** main owns them, so a resize is main's to run and the post pass
        // can be handed the resolve without anyone naming this pass.
        const SceneTargets* targets = nullptr;

        // Drawn from the pool by this pass and filled by it: the set names this
        // frame's input and uniform, so no one else knows what belongs in it.
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    PerFrame frames[kFramesInFlight];
};

// Effect: points the pass at what it draws into and at what the scene brings, and
//         makes the set each frame binds.
//
// It made the attachments until 09-05. main owns them now, which is what leaves this
// with no VulkanDevice argument: it creates descriptor sets and nothing else, and the
// pool knows its device.
//
// shadowMaps and not a ShadowPass, which was the same change made earlier: what this
// needs is one depth image per frame, and naming the pass that owns them let this
// function reach anything a shadow pass has. A signature is meant to state the
// requirement, not a place the requirement can be found in.
//
// gui stays a whole Gui on purpose. It is not a dependency of the same kind -- the
// panel exists to make features comparable while they are being understood, and it
// is deliberately a black box to whoever reads main. Its buffer already arrives
// through an accessor, which is asking rather than reaching in.
//
// Contract: shadowMaps holds kFramesInFlight entries, each an image the shadow pass
//           has created, frame for frame. Read at set-fill time and not stored: the
//           barrier that makes one readable belongs to the pass that writes it.
// Contract: gui must already be created -- binding 3 of each set names the buffer its
//           checkboxes write into.
// A resize touches no pass. This one's sets name nothing that changes -- the camera,
// the light, a shadow map and the panel's buffer -- and it holds pointers to targets
// whose contents are replaced under them. **The post pass's sets do not survive**:
// they name the resolve image, so RefreshPostProcessPass runs after every resize.

// Contract: cameras, lights and shadows hold kFramesInFlight entries and outlive this
//           pass. shadows is the same array the shadow pass was given, which is what
//           makes the matrix in binding 2 the one that drew the map in binding 3.
bool CreateScenePass(const Descriptors& descriptors,
                     const SceneTargets* const targets[kFramesInFlight],
                     const Mesh& mesh, const ShaderProgram& program,
                     const Pipeline& pipeline, const Pipeline& wirePipeline,
                     const Texture* const shadowMaps[kFramesInFlight],
                     const FrameCamera* cameras, const FrameLight* lights,
                     const FrameShadow* shadows,
                     const Gui& gui, ScenePass* out) noexcept;

// What recording one scene pass cost in state changes.
//
// Counted where it happens rather than worked out from the item list, so the number is
// what the command buffer actually got. These are what a sort order changes: the draws
// are fixed, the other two are not.
//
// They do not fall together. materialBinds reaches its floor -- the number of distinct
// materials -- as soon as equal materials are adjacent. cullChanges reaches its floor
// only if the order groups by cull first, which sorting on the material alone does not
// do even though cull is a function of it.
struct DrawStats {
    uint32_t draws = 0;
    uint32_t materialBinds = 0;
    uint32_t cullChanges = 0;
};

// What the panel decided about rasterization, as the three values this pass uses.
// Gathered into one struct so the signature does not grow a parameter per checkbox.
struct SceneRasterOptions {
    bool wireframe = false;
    bool depthTest = true;
    bool depthWrite = true;
    bool rasterizerDiscard = false;
    VkCullModeFlags cull = kCullFromMaterial;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
};

// Input:  the pass, the slot, this frame's list, what the panel decided, and where to
//         count what the recording cost
// Effect: appends the commands that draw this slot's colour and depth
//
// One mesh for every item: the spans in items index into it. Materials are bound only
// when the one a draw needs is not the one already bound, which is what sorting the
// list by material buys.
void RecordScenePass(const FrameSlot& slot, const ScenePass& scene,
                     const DrawList& draws, SceneRasterOptions raster,
                     DrawStats* stats) noexcept;

