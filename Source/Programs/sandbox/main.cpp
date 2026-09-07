// Assembly lives here: what is created in what order, and what one frame is given.
//
// Three layers meet in this file and nowhere else:
//
//   Vulkan/   how each resource is made and destroyed
//   Passes    what we draw, and in what order
//   here      which resources exist, who points at whom, and the loop
//
// Init and runtime obey different rules:
//
//               init        runtime (per frame)
//   runs        once        hundreds per second
//   heap        free        forbidden
//   log         free        floods if the condition persists
//   failure     unwind      drop the frame or recover
//
// The sections below are also the order they depend on each other, so nothing here
// moves up past what it reads.
//
//   file scope    Scene data     the loader, and the plain arrays it hands back
//                 Capture        one frame to a file, for comparing two builds
//
//   main, once    Declarations   in destruction order, which is not the fill order
//                 Display        a window and a GPU that can drive it
//                 Targets        what each pass draws into, all four, before a device
//                 Device         and past it, everything that needs one
//                 Passes         a program per shader pair, a pipeline per variant
//                 Scene          the mesh and the draw list
//                 Textures       the images the materials name
//                 Descriptors    the pool, sized by what the scene turned out to be
//                 Frames         per-frame values, then the passes, then the slots
//                 Frame state    what the loop carries across frames
//
//   main, loop    What to draw   clock, camera, light. Touches no GPU call
//                 Draw it        acquire, resize if asked, record, submit, present


#include "Bmp.h"                 // writing a captured frame to a file
#include "Config.h"
#include "Gltf.h"                // the asset, as the arrays a frame is built from
#include "Gui.h"                 // the panel, and the pass that draws it
#include "Passes.h"             // what we draw. main assembles it and hands it the frame
#include "PostProcessPass.h"      // named here: main makes its pipeline and its sets
#include "Renderer.h"            // everything that needs a device, grouped by kind
#include "ScenePass.h"            // named here: its targets, its pipelines and its sets
#include "ShadowPass.h"           // named here: its target, its pipeline and its sets
#include "Vertex.h"
#include "Vulkan/Attachments.h"   // main picks what we draw into, not the device layer
#include "Vulkan/Commands.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Frame.h"
#include "Vulkan/Mesh.h"
#include "Vulkan/Pipeline.h"
#include "Vulkan/Texture.h"
#include "Vulkan/Window.h"

#include <GLFW/glfw3.h>
#include <stb_image.h>

#include <algorithm>  // stable_sort, for the draw order
#include <memory>     // the renderer is too large for the stack
#include <cmath>      // cos, sin
#include <cstdio>     // fopen, for the capture's own file
#include <cstdlib>    // getenv, for the deterministic-capture switch
#include <iterator>   // std::size
#include <string>     // the texture path the glTF names
#include <vector>     // scene data is too big for the stack now

// One header at a time. <glm/ext.hpp> was dropped when vendoring (VERSION.md).
#include <glm/common.hpp>                  // clamp
#include <glm/geometric.hpp>               // normalize, cross
#include <glm/gtc/quaternion.hpp>          // angleAxis, and turning a vector by one
#include <glm/trigonometric.hpp>           // radians

// Scene data
// ============================================================================
//
// None of this is about Vulkan: everything here hands back plain arrays, which is what
// CreateMesh and CreateTextureFromPixels take.
//
// The loader is the only thing that fills a Vertex. CheckVertexInterface compares the
// .spv against the layout and sees whether a field is **supplied**, never whether it
// holds the right numbers -- which is why the loader refuses a file rather than
// filling a gap.


// Capture
// ============================================================================
//
// The image itself, read off the GPU -- not a screenshot of the window, which reads
// whatever is at those coordinates (the same binary measured 0.95% and 14.26% black on
// two runs). So two runs of one build are identical and a diff is only ever code.
//
// Contract: the subject is the presented image, at the window's size and with every
//           pass in it. Two captures compare only if taken the same way, so moving the
//           subject changes every recorded hash at once -- which is what happened when
//           it stopped being the scene's resolve.






// Effect: reads an image file into a texture, ready for a set to name it
//
// 4 channels forced: the shader samples a vec4 and there is no guaranteed 8-bit
// three-channel format.
//
// The format is the caller's and it is not a preference. Base colour is authored in
// sRGB and must say so; **a normal map is a direction, not a colour** -- read as SRGB
// every texel bends toward the flat normal, nothing reports it, and the picture is
// quietly wrong.
//
// The pixels live only for this call -- CreateTextureFromPixels stages and blocks.
static bool LoadTextureFile(const VulkanDevice& dev, const Commands& commands,
                            const char* path, VkFormat format, Texture* out) noexcept {
    int width = 0;
    int height = 0;
    int channelsInFile = 0;
    stbi_uc* pixels = stbi_load(path, &width, &height, &channelsInFile, 4);
    if (pixels == nullptr) {
        LOG("[img] cannot read %s (%s)\n", path, stbi_failure_reason());
        return false;
    }

    // A full chain, and TRANSFER_SRC because building it reads level i to write level
    // i + 1. Declared here rather than worked out inside the upload: how many levels an
    // image has is a property of the image, and a 1 x 1 stand-in asks for one.
    TextureDesc desc{{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                     format, VK_SAMPLE_COUNT_1_BIT,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT};
    desc.mipLevels = MipLevelsFor(desc.extent);
    const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    const bool uploaded = CreateTextureFromPixels(dev, commands, desc, pixels,
                                                  byteCount, out);
    stbi_image_free(pixels);
    if (uploaded) {
        LOG("[img] %s (%dx%d, %d channels in file)\n", path, width, height, channelsInFile);
    }
    return uploaded;
}

int main() {
    // Declarations -- in destruction order, which is not the fill order
    // ========================================================================
    //
    //   create   window/surface before device -- the surface picks the GPU
    //   destroy  the window's swapchain before device -- the device made it
    WindowSystem   windowSystem;   // dies last: glfwTerminate follows every window
    VulkanInstance inst;
    VulkanDevice   dev;
    Window         window;        // holds the swapchain, so it dies before dev
    Commands       commands;

    // Everything past this line needs a VkDevice, which is the whole reason the line
    // is here. What it holds and why it is grouped that way is in Renderer.h; the
    // order inside it is a destruction contract, not a preference.
    // **On the heap, and the reason is size rather than lifetime.** Every ShaderProgram
    // keeps what reflection found for each of its stages, which is about 65 KB of
    // fixed-size arrays, and twelve programs overflow the 1 MB stack a thread gets --
    // the symptom is an exit code of 0xC00000FD before anything is created.
    //
    // Still a local, so it is destroyed here rather than at program exit: everything in
    // it needs the device below to still exist, and that ordering is what this whole
    // block is written for.
    const std::unique_ptr<Renderer> renderer = std::make_unique<Renderer>();

    // Display -- a window, and a GPU that can drive it
    // ========================================================================

    // Set it to a path and one frame is written there and the program exits. Implies
    // fixed time -- a capture of a moving light compares against nothing -- and read up
    // here because it also decides whether a window is shown at all. What the capture
    // itself does with it is at the bottom of the loop.
    const char* const capturePath = std::getenv("LAMBDA_CAPTURE");

    // glfwInit is first only because windowSystem is declared first and so dies last.
    if (!InitWindowSystem(&windowSystem)) { return 1; }
    if (!CreateInstance(&inst)) { return 1; }
    if (!OpenWindow(inst, kWindowWidth, kWindowHeight, "Lambda Engine",
                    capturePath == nullptr, &window)) {
        return 1;
    }

    // Which GPU, and which of its queue families. Nothing about what we draw.
    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    // Must be sRGB: that encode happens nowhere else in the chain. The offscreen
    // colour still does not follow from it -- Attachments.cpp counts the cases.
    if (!ChooseSwapchainConfig(inst, selection.gpu, &window)) { return 1; }

    // Beside the format because it is the same kind of value: settled once, read on
    // every swapchain recreation. **Written here and not read down there** -- how many
    // images to rotate is our policy, and Vulkan/ includes nothing above itself.
    window.swapchainConfig.desiredImages = kDesiredSwapchainImages;

    // Not checked: false means minimized, and nothing below needs a size. The loop
    // asks again every frame.
    QuerySurfaceExtent(inst, selection.gpu, &window);

    // Targets -- what each pass draws into, all four described before anything exists
    // ========================================================================
    //
    // Still no device: describing a target needs none. Three kinds of value, in this
    // order because each needs the last -- received, chosen, answered.
    //
    // A pipeline bakes two of a TextureDesc's four fields. The two it leaves are the
    // two that mattered: the extent a projection answers to, and usage, in which
    // **SAMPLED marks an edge** -- exactly two of these images carry it, and those are
    // the two another pass reads.

    // Received -- the one of the four we do not write ourselves.
    const TextureDesc swapchainTarget = SwapchainTargetDesc(window);

    // Answered -- the two a caller cannot decide. Asked with our policy, which is why
    // the call is the renderer's and not the Vulkan layer's.
    TargetCapabilities caps;
    if (!RenderTargetCapabilities(inst, selection.gpu, &caps)) { return 1; }

    // The two that answer to the render size, from one extent so they cannot disagree.
    SceneTargetDescs sceneTargetDescs;
    GBufferTargetDescs gbufferDescs;
    DescribeSizedTargets(window.surfaceExtent, caps, &sceneTargetDescs, &gbufferDescs);

    const TextureDesc shadowTarget = MakeShadowTarget(caps);

    // A point light's shadow is a cube, so its target is one cube per light slot and the
    // depth the six faces are drawn with. Described here for the reason every other
    // target is: a pipeline is compiled against a face of them before any image exists.
    const TextureDesc pointShadowTarget = MakePointShadowTarget();
    const TextureDesc pointShadowDepth = MakePointShadowDepth(caps);
    const TextureDesc pointShadowFace = SliceDesc(pointShadowTarget);
    const TextureDesc pointShadowDepthFace = SliceDesc(pointShadowDepth);

    // One cube, and the 2D slice one of its faces is. The second is what the bake's
    // pipeline is compiled against -- a face is what that pass draws into.
    // Where the sun is. One fact, read by two things that must agree: the light the
    // scene is lit by, and the sky the environment maps are baked from. It was a
    // function of time, which made the shadows sweep across a sky that never moved --
    // and once there is an irradiance cube that inconsistency is not cosmetic, because
    // that cube is the same sun integrated over the hemisphere.
    //
    // y = 3.0 is measured: below it the arcades cut the sun off before the courtyard.
    const glm::vec3 kSunDirection = glm::normalize(glm::vec3{0.55f, 3.0f, 0.35f});

    const TextureDesc skyTarget = MakeSkyTarget();
    const TextureDesc skyFaceTarget = SliceDesc(skyTarget);
    const TextureDesc irradianceTarget = MakeIrradianceTarget();
    const TextureDesc irradianceFaceTarget = SliceDesc(irradianceTarget);
    const TextureDesc prefilterTarget = MakePrefilterTarget();
    const TextureDesc prefilterFaceTarget = SliceDesc(prefilterTarget);
    const TextureDesc brdfLutTarget = MakeBrdfLutTarget();

    // Device -- and past it, everything that needs one
    // ========================================================================

    // selection is absorbed here and not kept -- nothing below reads it.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }
    if (!CreateCommands(dev, &commands)) { return 1; }

    // Here and not with the passes: the descriptor pool has to be told about this
    // one's set before it is created, and the font it points at is uploaded here.
    if (!CreateGui(dev, commands, window, &renderer->guiPass)) { return 1; }

    // Passes -- a program per pair of shaders, a pipeline per variant of one
    // ========================================================================
    //
    // In the order a frame records them, and each names the target it draws into.
    // Four programs, five pipelines: the scene has two variants differing in
    // polygonMode, which is compiled in. Anything that is a register instead is
    // dynamic state and costs no second pipeline.

    // How each of those pieces of work runs is Pipelines.h's. What is handed over is
    // what a pipeline is compiled against -- the layouts of the two buffers anything
    // draws from, and the descs of the images anything draws into.
    PipelineSources pipelineSources;
    pipelineSources.meshLayout = VertexInput();
    pipelineSources.guiLayout = GuiVertexInput();
    pipelineSources.shadowDepth = &shadowTarget;
    pipelineSources.pointShadowFace = &pointShadowFace;
    pipelineSources.pointShadowDepth = &pointShadowDepthFace;
    pipelineSources.skyFace = &skyFaceTarget;
    pipelineSources.irradianceFace = &irradianceFaceTarget;
    pipelineSources.prefilterFace = &prefilterFaceTarget;
    pipelineSources.brdfLut = &brdfLutTarget;
    pipelineSources.sceneColor = &sceneTargetDescs.color;
    pipelineSources.sceneDepth = &sceneTargetDescs.depth;
    pipelineSources.swapchain = &swapchainTarget;
    pipelineSources.gAlbedo = &gbufferDescs.albedo;
    pipelineSources.gNormal = &gbufferDescs.normal;
    pipelineSources.gMaterial = &gbufferDescs.material;
    pipelineSources.gDepth = &gbufferDescs.depth;
    // The lighting pass draws into the image the scene pass resolves into, which is
    // what keeps everything after the middle the same on both paths.
    pipelineSources.sceneResolve = &sceneTargetDescs.resolve;

    // What a material is, from Passes.h. Both programs that draw a surface are held to
    // it, which is what lets one set of material sets fit both.
    const RequiredSet surfaceSets[] = {MaterialSet()};
    pipelineSources.surfaceSets = surfaceSets;
    pipelineSources.surfaceSetCount = static_cast<uint32_t>(std::size(surfaceSets));

    // And what each shared uniform block looks like inside, which every program is held
    // to. Written from the structs by offsetof, so this is the same declaration the
    // frame is uploaded from rather than a second copy of it.
    const ProgramRequirements blocks = SharedBlocks();
    pipelineSources.blocks = blocks.blocks;
    pipelineSources.blockCount = blocks.blockCount;
    pipelineSources.pushMembers = blocks.pushMembers;
    pipelineSources.pushMemberCount = blocks.pushMemberCount;
    if (!CreatePipelines(dev, pipelineSources, &renderer->pipelines)) { return 1; }

    // Scene -- the mesh and the draw list, from one file
    // ------------------------------------------------------------------------
    //
    // Heap, not the stack: Sponza is 192,496 vertices, which is 8.8 MB as Vertex[48].
    // Init may allocate freely (see the table at the top of this file); the frame loop
    // still may not, and nothing below the loop touches these again after the upload.
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<DrawItem> items;

    // No scene, no run. LoadGltf logs which way it failed; there is no second thing
    // to draw and putting one on screen would make a failed run look like a working
    // one. This is also why nothing below has to undo what a half-finished load
    // appended -- that path ends here.
    const char* const kScenePath = LAMBDA_ASSET_ROOT "/Sponza/Sponza.gltf";
    std::vector<uint32_t> itemMaterial;      // one per item, indexing materialSources
    std::vector<MaterialSource> materialSources;
    if (!LoadGltf(kScenePath, &vertices, &indices, &items,
                  &itemMaterial, &materialSources)) {
        return 1;
    }

    // Where the scene's one object is, as state rather than as a matrix
    //
    // glTF gives Sponza in centimetres and puts the scale on its one node. Applied here
    // rather than baked into the positions so the file stays the source of truth.
    //
    // **A stand-in, like kSceneCenter.** There is one of these because there is one
    // object and the loader drops the node transforms it reads; a Scene would hold one
    // per object. Named as a Transform anyway, because the renderer's side of the line
    // does not change when there are two -- it derives a model matrix from a transform
    // either way.
    constexpr float kSponzaScale = 0.008f;
    const Transform sceneTransform{glm::vec3{0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                   glm::vec3{kSponzaScale}};
    for (DrawItem& item : items) { SetDrawTransform(&item, sceneTransform); }

    // The same layout the scene pipeline was built with, said once here and compared
    // in CreateScenePass.
    const MeshDesc meshDesc{VertexInput(),
                            static_cast<uint32_t>(vertices.size()),
                            static_cast<uint32_t>(indices.size())};
    if (!CreateMesh(dev, commands, meshDesc, vertices.data(), indices.data(),
                    &renderer->mesh)) {
        return 1;
    }

    // Textures -- the images the materials name
    // ------------------------------------------------------------------------
    //
    // Three per material in runs: base colour, normal, metallic-roughness. One run
    // per material the file named -- every primitive names one, which the loader
    // insists on.
    //
    // A material missing an image gets a neutral texture and not a null: every binding
    // in the set has to point somewhere, and a null is a validation error at bind time.
    //
    // URIs are relative to the .gltf, per the spec.
    constexpr size_t kTexturesPerMaterial = 3;
    const uint32_t materialCount = static_cast<uint32_t>(materialSources.size());
    renderer->textures.resize(static_cast<size_t>(materialCount) * kTexturesPerMaterial);

    // One white texel, for a material that names no base colour. glTF says such a
    // material is its baseColorFactor alone, and white is the texture that multiplies
    // to exactly that -- a pattern here would be inventing detail the file does not
    // have. All 25 of this asset's materials name a base colour, so this draws nowhere.
    //
    // SRGB, because this is multiplied with the shader's output and has to be in the
    // same space as the render target. UNORM would brighten the result.
    const uint8_t whitePixel[4]{255, 255, 255, 255};
    const TextureDesc whiteDesc{{1, 1},
                                VK_FORMAT_R8G8B8A8_SRGB, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                    | VK_IMAGE_USAGE_SAMPLED_BIT};

    // The same idea in the other space: (128,128,255) decodes to +z, the geometric
    // normal unchanged. UNORM for the reason the loaded ones are -- this is a
    // direction. One of Sponza's 25 materials uses it.
    const uint8_t flatNormalPixels[4]{128, 128, 255, 255};
    const TextureDesc flatNormalDesc{{1, 1},
                                     VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                         | VK_IMAGE_USAGE_SAMPLED_BIT};

    // And once more for metallic-roughness. White again, for the same reason the base
    // colour's stand-in is: green and blue both multiply the factors, so 1.0 leaves
    // them alone. UNORM -- these are numbers, not a colour.
    //
    // 24 of Sponza's 25 materials name one, so this is drawn once.

    const TextureDesc numbersDesc{{1, 1},
                                  VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                      | VK_IMAGE_USAGE_SAMPLED_BIT};

    for (uint32_t i = 0; i < materialCount; ++i) {
        const MaterialSource& named = materialSources[i];
        const size_t first = static_cast<size_t>(i) * kTexturesPerMaterial;
        Texture& base = renderer->textures[first];
        Texture& normal = renderer->textures[first + 1];
        Texture& metalRough = renderer->textures[first + 2];

        if (!named.baseColor.empty()) {
            const std::string path = std::string(LAMBDA_ASSET_ROOT "/Sponza/")
                                   + named.baseColor;
            if (!LoadTextureFile(dev, commands, path.c_str(),
                                 VK_FORMAT_R8G8B8A8_SRGB, &base)) { return 1; }
        } else if (!CreateTextureFromPixels(dev, commands, whiteDesc, whitePixel,
                                            sizeof(whitePixel), &base)) {
            return 1;
        }

        if (!named.normal.empty()) {
            const std::string path = std::string(LAMBDA_ASSET_ROOT "/Sponza/")
                                   + named.normal;
            if (!LoadTextureFile(dev, commands, path.c_str(),
                                 VK_FORMAT_R8G8B8A8_UNORM, &normal)) { return 1; }
        } else if (!CreateTextureFromPixels(dev, commands, flatNormalDesc,
                                            flatNormalPixels, sizeof(flatNormalPixels),
                                            &normal)) {
            return 1;
        }

        // UNORM, not SRGB. These channels are roughness and metalness -- numbers the
        // shader multiplies, not light the eye sees. Reading them as SRGB would bend
        // every value and nothing would report it, which is the same trap the normal
        // map is in.
        if (!named.metallicRoughness.empty()) {
            const std::string path = std::string(LAMBDA_ASSET_ROOT "/Sponza/")
                                   + named.metallicRoughness;
            if (!LoadTextureFile(dev, commands, path.c_str(),
                                 VK_FORMAT_R8G8B8A8_UNORM, &metalRough)) { return 1; }
        } else if (!CreateTextureFromPixels(dev, commands, numbersDesc,
                                            whitePixel, sizeof(whitePixel),
                                            &metalRough)) {
            return 1;
        }
    }

    // Descriptors -- the pool, sized by what the scene turned out to be
    // ------------------------------------------------------------------------
    //
    // Here and not with the pipelines: the pool cannot be sized until the materials
    // are counted.
    //
    // Three claims from three different counts -- frames in flight for the two frame
    // sets, materials for the material set. How many descriptors each set holds is
    // read off the layout by CreateDescriptors, not written here.
    const SetRequest setRequests[] = {
        {&renderer->pipelines.shadowProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer->pipelines.pointShadowProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer->pipelines.sceneProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer->pipelines.sceneProgram.setLayouts[kMaterialSet], materialCount},
        // The deferred half. geometry's set 0 is two bindings with holes between them;
        // lighting's is the same five the scene's is, and its set 1 is the g-buffer
        // rather than a material -- which is why it is claimed here and the material
        // sets above are not counted twice. **geometry draws no material sets of its
        // own**: it speaks MaterialSet(), so the ones the scene pass uses fit it.
        {&renderer->pipelines.geometryProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer->pipelines.lightingProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer->pipelines.lightingProgram.setLayouts[kMaterialSet], kFramesInFlight},
        {&renderer->pipelines.irradianceProgram.setLayouts[kFrameSet], 1},
        {&renderer->pipelines.prefilterProgram.setLayouts[kFrameSet], 1},
        {&renderer->pipelines.skyProgram.setLayouts[kFrameSet], 2 * kFramesInFlight},
        {&renderer->pipelines.postProgram.setLayouts[kFrameSet], kFramesInFlight},
        // One, and counted by neither of the other two reasons: there is one font.
        {&renderer->pipelines.guiProgram.setLayouts[0], 1},
    };
    if (!CreateDescriptors(dev, setRequests,
                           static_cast<uint32_t>(std::size(setRequests)),
                           &renderer->descriptors)) { return 1; }

    // The pairs, as the two pointers a set is filled from. Built here rather than
    // stored, because textures owns them and this is only a way of reading it.
    std::vector<MaterialDesc> sources(materialCount);
    for (uint32_t i = 0; i < materialCount; ++i) {
        // The stand-in is last and the loader did not describe it: closed shapes, so
        // it culls like the opaque ones.
        const bool doubleSided = i < materialSources.size() && materialSources[i].doubleSided;
        // Same for the numbers: a white factor and no cutoff, which is what glTF means
        // by leaving both out.
        const MaterialParams params = i < materialSources.size() ? materialSources[i].params
                                                                 : MaterialParams{};
        // The cast is for the braced init: the enum is int, the flags field is
        // unsigned, and that counts as narrowing here.
        const size_t first = static_cast<size_t>(i) * kTexturesPerMaterial;
        sources[i] = {&renderer->textures[first],
                      &renderer->textures[first + 1],
                      &renderer->textures[first + 2],
                      params,
                      static_cast<VkCullModeFlags>(doubleSided ? VK_CULL_MODE_NONE
                                                               : VK_CULL_MODE_BACK_BIT)};
    }

    if (!CreateGuiSet(renderer->descriptors, renderer->pipelines.gui, swapchainTarget,
                      &renderer->guiPass)) { return 1; }

    renderer->materials.resize(materialCount);
    if (!CreateMaterials(dev, renderer->descriptors,
                         renderer->pipelines.sceneProgram.setLayouts[kMaterialSet], sources.data(),
                         materialCount, renderer->materials.data())) { return 1; }

    // Join the two halves the loader had to hand back separately. An index rather than
    // a pointer, so this survives renderer.materials moving in memory -- only its
    // length matters, and the recorder checks every index against it.
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].material = itemMaterial[i];
    }

    // The draw order. Only grouping matters, not which group leads: a bind happens
    // where two neighbours differ, so any order putting equal materials together
    // reaches the same count. Indices and not addresses, so two runs sort alike.
    //
    // Cull first, even though it is a function of the material. Sorting on the
    // material alone leaves the cull groups interleaved, and a material never
    // straddles two cull modes, so the coarser key costs nothing.
    //
    // Stable, so ties keep the file's order. Nothing depends on it yet -- blending is
    // off, so no draw has to come after another.
    const std::vector<Material>& materials = renderer->materials;
    std::stable_sort(items.begin(), items.end(),
                     [&materials](const DrawItem& a, const DrawItem& b) noexcept {
                         const VkCullModeFlags cullA = materials[a.material].cullMode;
                         const VkCullModeFlags cullB = materials[b.material].cullMode;
                         if (cullA != cullB) { return cullA < cullB; }
                         return a.material < b.material;
                     });

    // The items and the array their material index points into, paired once. Built
    // here rather than per frame because neither changes below this line, and built at
    // all because an index handed in without its array is the one way this goes wrong
    // without saying so.
    const DrawList drawList{items.data(), static_cast<uint32_t>(items.size()),
                            renderer->materials.data(), materialCount};

    // Frames -- per-frame values, then the passes that read them, then the slots
    // ------------------------------------------------------------------------
    //
    // In dependency order: the scene pass's sets name the shadow maps, the post pass's
    // name what the scene pass made. A slot owns none of it and only knows its index.
    //
    // The buffers come before both passes that read them, which is also why neither
    // owns them -- the shadow pass is created first and would have to outlive itself.
    if (!CreateFrameCameras(dev, renderer->cameras)) { return 1; }
    if (!CreateFrameLights(dev, renderer->lights)) { return 1; }
    if (!CreateFrameShadows(dev, renderer->shadows)) { return 1; }
    if (!CreateFramePointShadows(dev, renderer->pointShadows)) { return 1; }
    if (!CreateFrameViewOptions(dev, renderer->viewOptions)) { return 1; }

    // The shadow map, made here from the desc written at the top and handed to both
    // passes that touch it -- the one that draws it and the one that samples it. The
    // edge between them is this array, not a walk into whichever pass owned the image.
    // The desc is the identity and the images are this frame's copy of it. Not
    // &shadowMaps[0]->desc, which is one frame's copy and would make the identity
    // different every frame -- shadowTarget is what all of them were made from.
    // Made and filled before anything that reads it, and never touched again.
    if (!CreateTexture(dev, skyTarget, &renderer->skyCube)) { return 1; }
    if (!BakeSkyCube(dev, commands, renderer->pipelines.skyBake, kSunDirection,
                     &renderer->skyCube)) {
        return 1;
    }

    // From the sky, so after it. What every matte surface receives, worked out once
    // rather than per pixel per frame -- the integral has no other input.
    if (!CreateTexture(dev, irradianceTarget, &renderer->irradianceCube)) { return 1; }
    if (!BakeIrradianceCube(dev, commands, renderer->descriptors,
                            renderer->pipelines.irradianceBake,
                            renderer->skyCube, &renderer->irradianceCube)) {
        return 1;
    }

    // The specular half, and the table that says what a surface does with it. The first
    // is from the sky like the irradiance is; the second is from nothing at all and
    // would be the same file every run.
    if (!CreateTexture(dev, prefilterTarget, &renderer->prefilteredCube)) { return 1; }
    if (!BakePrefilterCube(dev, commands, renderer->descriptors,
                           renderer->pipelines.prefilterBake,
                           renderer->skyCube, &renderer->prefilteredCube)) {
        return 1;
    }
    if (!CreateTexture(dev, brdfLutTarget, &renderer->brdfLut)) { return 1; }
    if (!BakeBrdfLut(dev, commands, renderer->pipelines.brdfLutBake, &renderer->brdfLut)) {
        return 1;
    }

    const Texture* shadowMaps[kFramesInFlight]{};
    PassInput shadowMapInput{&shadowTarget, {}};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateTexture(dev, shadowTarget, &renderer->shadowMaps[i])) { return 1; }
        shadowMaps[i] = &renderer->shadowMaps[i];
        shadowMapInput.frames[i] = &renderer->shadowMaps[i];
    }

    if (!CreateShadowPass(renderer->descriptors, shadowTarget, shadowMaps,
                          renderer->mesh,
                          renderer->pipelines.shadow, renderer->shadows,
                          &renderer->shadowPass)) { return 1; }

    // The cubes and the depth they are drawn with, made here for the reason the 2D maps
    // are: one pass draws them and two sample them, so neither can own them.
    const Texture* pointShadowCubes[kFramesInFlight]{};
    const Texture* pointShadowDepths[kFramesInFlight]{};
    PassInput pointShadowInput{&pointShadowTarget, {}};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateTexture(dev, pointShadowTarget, &renderer->pointShadowCubes[i])
                || !CreateTexture(dev, pointShadowDepth,
                                  &renderer->pointShadowDepths[i])) {
            return 1;
        }
        pointShadowCubes[i] = &renderer->pointShadowCubes[i];
        pointShadowDepths[i] = &renderer->pointShadowDepths[i];
        pointShadowInput.frames[i] = &renderer->pointShadowCubes[i];
    }

    if (!CreatePointShadowPass(renderer->descriptors, pointShadowTarget, pointShadowCubes,
                               pointShadowDepth, pointShadowDepths, renderer->mesh,
                               renderer->pipelines.pointShadow, renderer->pointShadows,
                               &renderer->pointShadowPass)) { return 1; }
    // The scene's three, made here and named here. The pass draws into them, the post
    // pass samples the resolve, and the capture reads the same image -- three readers
    // and no pass in the middle of any of them.
    const SceneTargets* sceneTargets[kFramesInFlight]{};
    const Texture* sceneColor[kFramesInFlight]{};

    // The multisample one, which is what the forward path's sky draws into -- the
    // deferred path's sky draws into the resolve instead, and that is the only
    // difference between the two sky passes.
    const Texture* sceneColorMs[kFramesInFlight]{};

    // What leaves the middle, whichever of the two paths drew it. The scene pass
    // resolves into this and the lighting pass draws into it, so the identity is one
    // desc and both of them name it.
    PassInput sceneColorInput{&sceneTargetDescs.resolve, {}};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateSceneTargets(dev, sceneTargetDescs, &renderer->sceneTargets[i])) {
            return 1;
        }
        sceneTargets[i] = &renderer->sceneTargets[i];

        // resolve and not color: a multisample image cannot be sampled.
        sceneColor[i] = &renderer->sceneTargets[i].resolve;
        sceneColorInput.frames[i] = &renderer->sceneTargets[i].resolve;
        sceneColorMs[i] = &renderer->sceneTargets[i].color;
    }

    // What set 0 holds, said once. Each program takes the subset it declared, so the
    // geometry pass gets the same value and reflection leaves the three it does not
    // read as holes.
    const FrameSetSources frameSet{renderer->cameras, renderer->lights, renderer->shadows,
                                   shadowMapInput, renderer->viewOptions,
                                   &renderer->skyCube, &renderer->irradianceCube,
                                   &renderer->prefilteredCube, &renderer->brdfLut,
                                   pointShadowInput};

    if (!CreateScenePass(renderer->descriptors, sceneTargetDescs, sceneTargets,
                         renderer->mesh, renderer->pipelines.scene,
                         renderer->pipelines.sceneWire,
                         frameSet, &renderer->scenePass)) { return 1; }
    if (!CreateSkyPass(renderer->descriptors, sceneTargetDescs.color, sceneColorMs,
                       renderer->pipelines.skyForward, frameSet,
                       &renderer->skyForwardPass)) { return 1; }

    if (!CreatePostProcessPass(renderer->descriptors, sceneColorInput, swapchainTarget,
                               renderer->pipelines.post,
                               &renderer->postPass)) {
        return 1;
    }

    // The deferred middle. Both chains exist from here on and neither is rebuilt when
    // the panel switches -- what the switch changes is which of them RecordFrame
    // names.
    const GBufferTargets* gbuffers[kFramesInFlight]{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateGBufferTargets(dev, gbufferDescs, &renderer->gbuffers[i])) {
            return 1;
        }
        gbuffers[i] = &renderer->gbuffers[i];
    }

    if (!CreateGeometryPass(renderer->descriptors, gbufferDescs, gbuffers,
                            renderer->mesh, renderer->pipelines.geometry,
                            renderer->pipelines.geometryWire,
                            frameSet, &renderer->geometryPass)) { return 1; }

    // sceneColor is the scene pass's resolve, and this pass draws into it rather than
    // reading it. Only one of the two ever writes it in a frame.
    if (!CreateLightingPass(renderer->descriptors, gbufferDescs, gbuffers,
                            sceneTargetDescs.resolve, sceneColor,
                            renderer->pipelines.lighting,
                            frameSet, &renderer->lightingPass)) { return 1; }

    if (!CreateSkyPass(renderer->descriptors, sceneTargetDescs.resolve, sceneColor,
                       renderer->pipelines.skyDeferred, frameSet,
                       &renderer->skyDeferredPass)) { return 1; }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateFrameSlot(dev, commands, i, &renderer->slots[i])) { return 1; }
        if (!CreateGpuTimer(dev, commands, kTimestampsPerFrame, &renderer->timers[i])) {
            return 1;
        }
    }


    LOG("close the window to exit. The panel switches features off.\n");

    // Frame state -- what the loop carries across frames
    // ------------------------------------------------------------------------
    //
    // Not a struct: these are the angles the keys change, and the boundary already has
    // a type -- the loop hands the renderer a Transform built from eye and the two of
    // them. Grouping them here would name the same thing twice.
    uint32_t slotIndex = 0;       // which slot this frame borrows
    double lastTime = glfwGetTime();

    // What the frustum leaves, refilled every frame. Declared here so the allocation
    // happens once: clear() keeps the capacity and the list never grows past items.
    std::vector<DrawItem> visibleItems;
    visibleItems.reserve(items.size());

    // Where the scene is, for the shadow box to cover
    //
    // **A stand-in, and worth naming as one.** This is not a policy the renderer chose;
    // it is a fact about the scene, standing in for something a scene would say. There
    // is one of it because there is one Sponza and it does not move, and sceneTransform
    // above is the same fact wearing different clothes. The renderer takes it as an
    // argument rather than holding it, so neither of those becomes part of a type.
    //
    // Fixed rather than fitted to the camera, so the box covers this scene and nothing
    // larger; scene.frag returns "lit" outside it.
    constexpr glm::vec3 kSceneCenter{0.0f, 3.0f, 0.0f};

    // What the item order costs in state changes. Outside the loop because the panel
    // is built before RecordFrame fills it, so what it shows is the last frame's --
    // honest only because the list does not change between frames.
    DrawStats drawStats;
    bool loggedDrawStats = false;
    bool loggedGpuTimings = false;

    // The light is the only thing here that reads absolute time, so two runs never
    // draw the same picture unless this stops it. Environment variables rather than
    // Config constants: a capture and a person running this want opposite values.
    //
    // Read once, and dt still comes from the real clock or the camera stops answering.
    const bool fixedTime =
        std::getenv("LAMBDA_FIXED_TIME") != nullptr || capturePath != nullptr;

    // Which frame a capture is of. 1 and not 0 because the panel is not in frame 0 --
    // see the capture itself, below the submit.
    constexpr uint32_t kCaptureFrame = 1;
    uint32_t framesDrawn = 0;

    // Inside the atrium, looking along it.
    glm::vec3 eye{-7.0f, 5.5f, 0.0f};
    float yaw = 0.0f;             // 0 looks down +x, per the forward expression below
    float pitch = 4.0f;           // a little of the open roof, so a capture covers the sky         // the atrium floor, from the height of its gallery

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // The window's size, asked of the surface, and the minimize check with it:
        // 0x0 is what a minimized surface reports. Skipping here is what keeps
        // EnsureSwapchain from waiting and recreating every iteration with no present
        // to pace it.
        //
        // The clock resets after waking: the sleep is not a frame, and counting it
        // would teleport the camera on the next dt.
        if (!QuerySurfaceExtent(inst, dev.gpu, &window)) {
            glfwWaitEvents();
            lastTime = glfwGetTime();
            continue;
        }

        // Resize -- if the policy says the targets should be another size
        //
        // Above the acquire, because the query above is where a window's size comes
        // from. vkDeviceWaitIdle and not a fence: a fence covers one slot, and these
        // images belong to every slot.
        const VkExtent2D wanted = RenderExtentFor(window.surfaceExtent);

        // Compared against the desc and not against a copy of it: the desc is where
        // the current size lives, so there is no second number to keep in step.
        const VkExtent2D current = sceneTargetDescs.color.extent;

        if (wanted.width != current.width || wanted.height != current.height) {
            dev.table.vkDeviceWaitIdle(dev.handle);

            // Described again at the new size, by the same call that described them
            // the first time. Only the extent differs, so the pipelines stand and the
            // scene pass's pointers still name the right objects.
            DescribeSizedTargets(window.surfaceExtent, caps,
                                 &sceneTargetDescs, &gbufferDescs);

            bool remade = true;
            for (uint32_t i = 0; i < kFramesInFlight && remade; ++i) {
                remade = ResizeSceneTargets(dev, sceneTargetDescs,
                                            &renderer->sceneTargets[i])
                      && ResizeGBufferTargets(dev, gbufferDescs,
                                              &renderer->gbuffers[i]);
            }
            if (!remade) { break; }

            // The sets that name what was just destroyed. Two now: the post pass
            // reads the resolve, and the lighting pass's second set names all four
            // g-buffer views. The lighting pass's first set survives -- it names
            // buffers and the shadow map, and a resize touches neither.
            RefreshPostProcessPass(renderer->descriptors, &renderer->postPass);
            RefreshLightingPass(renderer->descriptors, &renderer->lightingPass);
            LOG("[render] targets now %ux%u\n", wanted.width, wanted.height);
        }

        // What to draw
        // --------------------------------------------------------------------
        //
        // No GPU call in any of it, so it could run while minimized. What comes out is
        // state -- a camera, a light, a list -- and the next section sends it.

        // Clock
        //
        // The gap only. **Nothing in a frame reads absolute time any more** -- the sun
        // was the last thing that did, and it stopped when it became a scene fact the
        // environment is baked from. What is left is how far the keys move the camera.
        //
        // Still fixed under a capture, because the panel prints a frame time and the
        // machine's speed would otherwise reach the picture. 1/60 and not 0 -- a zero
        // gap is a frame nothing could have moved in.
        const double now = glfwGetTime();
        const float dt = fixedTime ? 1.0f / 60.0f
                                   : static_cast<float>(now - lastTime);
        lastTime = now;

        // Camera -- input to a view
        //
        // glfwGetKey reads the state glfwPollEvents cached, so this block gives the
        // same answer wherever it sits. Speeds are multiplied by dt, or the frame rate
        // becomes the speed.

        const auto held = [&](int key) {
            return glfwGetKey(window.handle, key) == GLFW_PRESS;
        };

        if (held(GLFW_KEY_LEFT))  { yaw   -= kTurnSpeed * dt; }
        if (held(GLFW_KEY_RIGHT)) { yaw   += kTurnSpeed * dt; }
        if (held(GLFW_KEY_UP))    { pitch += kTurnSpeed * dt; }
        if (held(GLFW_KEY_DOWN))  { pitch -= kTurnSpeed * dt; }

        // A choice, not a requirement: nothing below crosses two vectors, so +-90 is
        // a representable orientation. It stays because these four keys are not for
        // tipping past vertical.
        pitch = glm::clamp(pitch, -89.0f, 89.0f);

        // Built from the two angles each frame and not accumulated: a running product
        // of small turns drifts and needs renormalizing, and these two angles are
        // already the whole of what the keys change.
        //
        // The quarter turn is where two conventions meet: yaw is measured from +x (see
        // its initial value) and a quaternion's identity looks down -z.
        constexpr float kYawFromIdentity = 90.0f;
        const glm::quat orientation =
              glm::angleAxis(glm::radians(-(yaw + kYawFromIdentity)), kWorldUp)
            * glm::angleAxis(glm::radians(pitch), glm::vec3{1.0f, 0.0f, 0.0f});

        // Read back out of the orientation rather than kept beside it, so neither can
        // drift from it. right stays horizontal: pitch turns about the local x, which
        // leaves the axis the yaw put it on.
        const glm::vec3 forward = orientation * glm::vec3{0.0f, 0.0f, -1.0f};
        const glm::vec3 right   = orientation * glm::vec3{1.0f, 0.0f, 0.0f};

        if (held(GLFW_KEY_W)) { eye += forward * kMoveSpeed * dt; }
        if (held(GLFW_KEY_S)) { eye -= forward * kMoveSpeed * dt; }
        if (held(GLFW_KEY_D)) { eye += right   * kMoveSpeed * dt; }
        if (held(GLFW_KEY_A)) { eye -= right   * kMoveSpeed * dt; }
        if (held(GLFW_KEY_E)) { eye += kWorldUp * kMoveSpeed * dt; }
        if (held(GLFW_KEY_Q)) { eye -= kWorldUp * kMoveSpeed * dt; }


        // Light -- state, and only state
        //
        // The same direction the sky was baked from, because they are one fact. Moving
        // it means baking the sky, the irradiance and the prefiltered cube again: the
        // environment would stop being an input of the frame and become something a
        // frame produces, which is the line the frame graph draws.
        // The scene's lights. The first is the sun the sky was baked from and the
        // only one with a shadow map; the two after it are things in the room, which is
        // the difference the kind makes -- they have somewhere to be.
        LightState lights[3]{};

        lights[0].kind = LightKind::Directional;
        lights[0].direction = kSunDirection;
        // Times pi, and the reason is the BRDF rather than the light. A Lambertian
        // surface returns albedo / pi of what arrives, and until the shading used a
        // microfacet model that division was simply left out -- so every light was
        // implicitly pi times what it said. Putting the factor where it belongs and
        // scaling the lights by it keeps the same picture with the terms now correct.
        constexpr float kRadiance = 3.14159265f;

        lights[0].color = glm::vec3{1.0f, 0.95f, 0.9f} * kRadiance;

        lights[1].kind = LightKind::Spot;
        lights[1].position = kSceneCenter + glm::vec3{5.0f, 7.5f, 0.0f};
        lights[1].direction = glm::vec3{0.0f, 1.0f, 0.0f};   // pointing down
        lights[1].color = glm::vec3{6.0f, 7.0f, 12.0f} * kRadiance;   // a cool one
        lights[1].innerCos = 0.94f;
        lights[1].outerCos = 0.80f;
        lights[1].range = 20.0f;

        // Last, because it casts no shadow and the casters are a prefix.
        lights[2].kind = LightKind::Point;
        // Low, and close to the colonnade the camera is looking down. Two facts decide
        // where a point light has to be for anyone to see what it does. Its falloff is
        // inverse square with a range window, so past three or four units it is a few
        // percent of the sun; and a shadow needs a surface the light cannot see and the
        // camera can, which a light sitting at the camera never has. Beside the near
        // columns at floor height satisfies both.
        lights[2].position = kSceneCenter + glm::vec3{-3.5f, 2.0f, -1.8f};
        lights[2].color = glm::vec3{9.0f, 3.6f, 1.2f} * kRadiance;   // a warm lamp
        lights[2].range = 14.0f;

        // Which lights get a map, and it is a prefix rather than a set: the ones that
        // can cast are put first so "how many" is the whole answer. A point light
        // cannot -- its map would be a cube and six passes, not a layer -- so it is
        // last and the count stops before it.
        //
        // Ordered here, in the one place that knows what the scene's lights are.
        uint32_t shadowCasters = 0;
        for (uint32_t i = 0; i < std::size(lights); ++i) {
            if (lights[i].kind == LightKind::Point) { break; }
            lights[i].castsShadow = true;
            shadowCasters = i + 1;
        }

        // And the point lights, which get a cube instead. Not a prefix: they are what
        // the 2D pass stopped at, so they are named by index rather than counted.
        uint32_t pointLights[kMaxLights]{};
        uint32_t pointLightCount = 0;
        for (uint32_t i = 0; i < std::size(lights) && i < kMaxLights; ++i) {
            if (lights[i].kind != LightKind::Point) { continue; }
            lights[i].castsShadow = true;
            pointLights[pointLightCount++] = i;
        }
        // Fill this frame's share of the pass
        //
        // Assignment only, so it belongs up here: what reaches the GPU, and when, is
        // RecordFrame's. Through slot.index and not slotIndex: recording picks the
        // pass's frame the same way.
        FrameSlot& slot = renderer->slots[slotIndex];

        // State, not a matrix. viewPos below comes back out of it rather than being
        // copied beside it -- one camera, one place its position is written down.
        const CameraState camera{{eye, orientation}, kFovDegrees};

        // Named and not positional, in all three: each block has two adjacent fields
        // of the same type, and reflection compares the layout rather than which matrix
        // went in which slot. C++20 requires designators in declaration order, so a
        // transposition is a compile error instead of a wrong picture.
        //
        // proj is rebuilt here and lightProj is not: this one answers to a target that
        // resizes and a fov the app could change, and ShadowProjectionFor takes nothing
        // that moves.
        renderer->cameras[slot.index].value =
            {.view = ViewFromPose(camera.pose),
             .proj = ProjectionFor(camera.fovDegrees, sceneTargetDescs.color.extent),
             .viewPos = glm::vec4{camera.pose.position, 1.0f}};

        // What reaches a surface, and where its shadow map was drawn from. The second
        // is made here rather than held: it turns with the light every frame, while
        // lightProj above does not move at all.
        // The ambient is the scene's and not any one light's, which is why it is an
        // argument here rather than a field of the first of them.
        if (!FillLights(lights, static_cast<uint32_t>(std::size(lights)),
                        glm::vec3{0.15f} * kRadiance,
                        &renderer->lights[slot.index].value)) {
            break;
        }
        // Both from the light itself now, because both answers differ by its kind: a
        // spot looks from where it is with a perspective, a directional light from a
        // point invented far enough back with an orthographic box.
        // One pair per light, in the same order, so index i is index i everywhere.
        // The ones with no map are written anyway and never read -- writing them costs
        // a memcpy and skipping them would need a second thing saying which.
        for (uint32_t i = 0; i < std::size(lights); ++i) {
            renderer->shadows[slot.index].value.lights[i] =
                {ShadowView(lights[i], kSceneCenter),
                 ShadowProjectionFor(lights[i], shadowTarget.extent)};
        }

        // Six views per point light, and where each of them is. The fragment stage of
        // the cube pass reads the position out of the same buffer, so the distance it
        // writes and the distance the shading pass compares against come from one place.
        FillPointShadows(lights, static_cast<uint32_t>(std::size(lights)),
                         &renderer->pointShadows[slot.index].value);

        // Draw it
        // --------------------------------------------------------------------
        //
        // Acquire, resize if the policy asks, record four passes, submit, present.
        // From the acquire on, a failure breaks rather than continues: the semaphore
        // and the fence are already spoken for.

        // Where this frame goes. Lives until present and no further, and the loop
        // only carries it -- BeginFrame is what pairs it with this slot.
        FrameTarget target;
        const FrameResult begun = BeginFrame(dev, &window, slot, &target);
        if (begun == FrameResult::Fatal) { break; }

        if (begun == FrameResult::Skip) { continue; }

        // What the GPU reported for the last frame that ran on this slot. Here because
        // BeginFrame waited on that submit's fence, which is what makes the results
        // readable; earlier and there would be nothing to read.
        PassTimings gpuTimings;
        ReadPassTimings(renderer->timers[slot.index], &gpuTimings);

        // Once, the first time there is anything to report, so the numbers reach the
        // console without the panel being opened. A capture run does not reach this: it
        // exits at frame kCaptureFrame, before this slot's pool has been written and
        // read once.
        if (!loggedGpuTimings && gpuTimings.totalMs > 0.0) {
            loggedGpuTimings = true;
            LOG("[gpu] pass times, ms:");
            for (uint32_t i = 0; i < kTimedPassCount; ++i) {
                if (!gpuTimings.ran[i]) { continue; }
                LOG("  %s %.3f", TimedPassName(static_cast<TimedPass>(i)),
                    gpuTimings.ms[i]);
            }
            LOG("  total %.3f\n", gpuTimings.totalMs);
        }

        // The panel, after the acquire because it reports the image this frame got.
        // Nothing here touches the GPU -- it only fills a draw list that
        // RecordGuiPass reads. Below the Skip return on purpose: a frame that is not
        // recorded would leave that list stale.
        GuiFrameInfo guiInfo;
        guiInfo.frameSeconds = dt;
        guiInfo.itemCount = static_cast<uint32_t>(items.size());
        guiInfo.materialCount = materialCount;
        guiInfo.recordedDraws = drawStats.draws;
        guiInfo.culledDraws = drawStats.culled;
        guiInfo.gpuTimings = &gpuTimings;
        guiInfo.materialBinds = drawStats.materialBinds;
        guiInfo.cullChanges = drawStats.cullChanges;
        guiInfo.descriptors = &renderer->descriptors;
        guiInfo.sceneProgram = &renderer->pipelines.sceneProgram;
        guiInfo.postProgram = &renderer->pipelines.postProgram;
        guiInfo.guiProgram = &renderer->pipelines.guiProgram;
        guiInfo.scenePipeline = &renderer->pipelines.scene;
        guiInfo.postPipeline = &renderer->pipelines.post;
        guiInfo.cameraBytes = static_cast<uint32_t>(sizeof(CameraUniform));
        guiInfo.lightBytes = static_cast<uint32_t>(sizeof(LightUniform)
                                                   + sizeof(ShadowUniform));
        guiInfo.pushBytes = static_cast<uint32_t>(sizeof(PushConstants));
        guiInfo.vertexStride = renderer->mesh.desc.vertexLayout.stride;
        guiInfo.vertexAttributes = renderer->mesh.desc.vertexLayout.attributeCount;
        guiInfo.framesInFlight = kFramesInFlight;
        guiInfo.mesh = &renderer->mesh;
        guiInfo.guiPipeline = &renderer->pipelines.gui;
        guiInfo.slotIndex = slot.index;
        guiInfo.sceneColor = &renderer->sceneTargets[slot.index].color;
        guiInfo.sceneResolve = &renderer->sceneTargets[slot.index].resolve;
        guiInfo.sceneDepth = &renderer->sceneTargets[slot.index].depth;
        guiInfo.frameTarget = target.texture;
        BuildGui(&renderer->guiPass, guiInfo);


        // The values written above, into the buffers the sets already name. Here and
        // not inside RecordFrame: it is a memcpy per value, not a command.
        UploadFrameValues(slot, renderer->cameras, renderer->lights, renderer->shadows,
                          renderer->pointShadows, renderer->viewOptions,
                          renderer->guiPass);

        // Only the texture: recording has no use for the rest of the target.
        //
        // Reset rather than declared here: the counters add up, and the panel above
        // read last frame's values before this line overwrites them.
        drawStats = DrawStats{};

        // What this camera can see, rebuilt every frame because the camera moves. The
        // frustum comes from the same two matrices the vertex stage will multiply by,
        // read back out of the block that was just written, so the test cannot drift
        // from what is drawn.
        //
        // A filtered copy rather than a flag on the item: the recorder walks a list and
        // binds a material where two neighbours differ, and a skipped item in the middle
        // of that walk would still have to be examined. The vector keeps its capacity
        // across frames, so this allocates once.
        const CameraUniform& sent = renderer->cameras[slot.index].value;
        const Frustum frustum = FrustumFrom(sent.proj * sent.view);
        visibleItems.clear();
        for (const DrawItem& item : items) {
            if (IsVisible(frustum, item)) { visibleItems.push_back(item); }
        }
        drawStats.culled = static_cast<uint32_t>(items.size() - visibleItems.size());

        const DrawList visibleList{visibleItems.data(),
                                   static_cast<uint32_t>(visibleItems.size()),
                                   renderer->materials.data(), materialCount};

        if (!RecordFrame(slot, renderer->shadowPass, shadowCasters,
                         renderer->pointShadowPass, pointLights, pointLightCount,
                         renderer->skyForwardPass, renderer->skyDeferredPass,
                         renderer->scenePass,
                         renderer->geometryPass, renderer->lightingPass,
                         renderer->postPass, renderer->guiPass,
                         *target.texture, visibleList, drawList,
                         renderer->timers[slot.index], &drawStats)) {
            break;
        }

        // Once. The list does not change between frames, so neither do these -- and a
        // line per frame would bury the one number that matters.
        if (!loggedDrawStats) {
            LOG("[draw] %u draws (%u culled), %u material binds, %u cull changes\n",
                drawStats.draws, drawStats.culled, drawStats.materialBinds,
                drawStats.cullChanges);
            loggedDrawStats = true;
        }

        // Frame.h holds the reason submit and present are separate.
        if (!SubmitFrame(dev, slot, target)) {
            break;
        }

        // One frame, then out. Everything the picture depends on is settled before the
        // loop -- textures uploaded, camera at its start -- so waiting longer only adds
        // whatever the clock and the keyboard did meanwhile.
        //
        // Between submit and present, and the position is the point. The subject is the
        // image the frame actually shows, so every pass is in it -- the post pass's
        // letterboxing and the panel included. Reading an earlier image left both out,
        // and a change to either moved nothing.
        //
        // Not the first frame, and that is measured rather than assumed: ImGui builds
        // no vertices for a window on the frame it is created, so a capture of frame 0
        // has no panel in it however late in the frame it is taken. The clock is fixed,
        // nothing is typed, and every draw list is rebuilt from scratch -- so frame 1
        // is as reproducible as frame 0 was.
        //
        // The wait is for this frame's own submit: these commands are still writing the
        // image ReadTexturePixels copies from. Present is skipped afterwards, because
        // the loop ends here and nothing would see it.
        framesDrawn += 1;
        if (capturePath != nullptr && framesDrawn > kCaptureFrame) {
            dev.table.vkDeviceWaitIdle(dev.handle);
            const Texture& shot = *target.texture;

            // Contract: WriteBmp reads red first, and ReadTexturePixels hands back the
            //           image's own channel order -- BGRA included. Refused rather
            //           than swizzled: a swizzle would be a branch this machine never
            //           takes.
            if (shot.desc.format != VK_FORMAT_R8G8B8A8_SRGB
                    && shot.desc.format != VK_FORMAT_R8G8B8A8_UNORM) {
                LOG("[capture] format %d is not red-first; BMP would swap R and B\n",
                    static_cast<int>(shot.desc.format));
                break;
            }

            // PRESENT_SRC because that is where RecordFrame leaves it -- the layout a
            // presentable image is in when the frame is done with it.
            std::vector<uint8_t> pixels;
            if (ReadTexturePixels(dev, commands, shot,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &pixels)
                    && WriteBmp(capturePath, shot.desc.extent.width,
                                shot.desc.extent.height, pixels.data())) {
                LOG("[capture] %ux%u -> %s\n",
                    shot.desc.extent.width, shot.desc.extent.height, capturePath);
            }
            break;
        }

        if (!PresentFrame(dev, &window, target)) {
            break;
        }

        slotIndex = (slotIndex + 1) % kFramesInFlight;
    }

    // Destructors run in reverse declaration order. This wait stays because
    // ~VulkanDevice waits only after every other destructor has already run, and the
    // swapchain and command pool may still be in use by the GPU.
    dev.table.vkDeviceWaitIdle(dev.handle);

    LOG("[vk] clean shutdown\n");
    return 0;
}
