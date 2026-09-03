// Assembly lives here: what is created in what order, and what one frame is given.
//
// Three layers meet in this file and nowhere else:
//
//   Vulkan/   how each resource is made and destroyed
//   Passes    what we draw, and in what order
//   here      which resources exist, who points at whom, and the loop
//
// Init and runtime obey different rules, and that is what split the files:
//
//               init        runtime (per frame)
//   runs        once        hundreds per second
//   heap        free        forbidden
//   log         free        floods if the condition persists
//   failure     unwind      drop the frame or recover


#include "Config.h"
#include "Gui.h"                 // the panel, and the pass that draws it
#include "Passes.h"             // what we draw. main assembles it and hands it the frame
#include "Renderer.h"            // everything that needs a device, grouped by kind
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
#include <cgltf.h>
#include <stb_image.h>

#include <algorithm>  // stable_sort, for the draw order
#include <cmath>      // cos, sin
#include <cstdio>     // fopen, to test for the asset before loading it
#include <cstdlib>    // getenv, for the deterministic-capture switch
#include <iterator>   // std::size
#include <string>     // the texture path the glTF names
#include <vector>     // scene data is too big for the stack now

// One header at a time. <glm/ext.hpp> was dropped when vendoring (VERSION.md).
#include <glm/common.hpp>                  // clamp
#include <glm/ext/matrix_clip_space.hpp>   // perspective
#include <glm/ext/matrix_transform.hpp>    // rotate, translate, scale, lookAt
#include <glm/geometric.hpp>               // normalize, cross
#include <glm/trigonometric.hpp>           // radians, cos, sin

// Scene data
// ============================================================================
//
// None of this is about Vulkan: everything here hands back plain arrays, which is what
// CreateMesh and CreateTextureFromPixels take. The glTF loader fills the same ones the
// generated sphere does -- that is the whole reason MakeSphere stays.

// A UV sphere. On a unit sphere the position is also the normal, which is the whole
// reason this shape shows lighting.
//
// Output: vertices[(stacks+1) * (slices+1)], indices[stacks * slices * 6]
static void MakeSphere(uint32_t stacks, uint32_t slices, float radius,
                       Vertex* vertices, uint16_t* indices) noexcept {
    for (uint32_t stack = 0; stack <= stacks; ++stack) {
        // phi from the +y pole down to -y; theta all the way around.
        const float phi = 3.14159265f * static_cast<float>(stack) / stacks;
        for (uint32_t slice = 0; slice <= slices; ++slice) {
            const float theta = 6.28318531f * static_cast<float>(slice) / slices;
            const float sinPhi = std::sin(phi);
            const float nx = sinPhi * std::cos(theta);
            const float ny = std::cos(phi);
            const float nz = sinPhi * std::sin(theta);

            Vertex& v = vertices[stack * (slices + 1) + slice];
            v.position[0] = nx * radius;
            v.position[1] = ny * radius;
            v.position[2] = nz * radius;
            v.normal[0] = nx; v.normal[1] = ny; v.normal[2] = nz;
            v.uv[0] = static_cast<float>(slice) / slices;
            v.uv[1] = static_cast<float>(stack) / stacks;

            // Along increasing theta -- the direction u grows in, which is what a
            // normal map will expect.
            v.tangent[0] = -std::sin(theta);
            v.tangent[1] = 0.0f;
            v.tangent[2] =  std::cos(theta);
            v.tangent[3] = 1.0f;
        }
    }

    // Two triangles per quad. CCW seen from outside, which is what BACK culling wants.
    uint32_t written = 0;
    for (uint32_t stack = 0; stack < stacks; ++stack) {
        for (uint32_t slice = 0; slice < slices; ++slice) {
            const uint16_t top = static_cast<uint16_t>(stack * (slices + 1) + slice);
            const uint16_t bottom = static_cast<uint16_t>(top + slices + 1);
            indices[written++] = top;
            indices[written++] = bottom;
            indices[written++] = static_cast<uint16_t>(top + 1);
            indices[written++] = static_cast<uint16_t>(top + 1);
            indices[written++] = bottom;
            indices[written++] = static_cast<uint16_t>(bottom + 1);
        }
    }
}

// What one material names, as file paths. A pair rather than two lists so the two
// cannot drift apart, and so the dedup key is one comparison.
//
// Empty means the material named none, and the caller substitutes: a checker for the
// base colour, a flat normal for the other.
// What the loader found for one material, and the key two of them are compared on.
//
// Every field that makes two materials different has to be in here. Cull is in it for
// that reason and not because a name is an image: two glTF materials naming the same
// two images but differing in double_sided are two materials, and leaving cull out of
// the key would quietly make them one. That is what makes Material::cullMode a
// function of the material rather than a coincidence -- in Sponza it costs no extra
// entry, because no two share a pair.
//
// The factors are the next thing missing here, and they collapse the same way today.
struct MaterialSource {
    std::string baseColor;
    std::string normal;
    bool doubleSided = false;
};

// Capture
// ============================================================================
//
// What the scene pass produced, as a file, read off the GPU. The alternative was a
// screenshot of the window, which reads whatever is at those coordinates -- the same
// binary measured 0.95% and 14.26% black on two runs. This reads the image itself, so
// two runs of one build are identical by construction and a diff is only ever code.
//
// colorResolve is the subject: at kRenderExtent whatever the window is doing, and
// before the panel is drawn on top.

static void Put32(uint8_t* at, uint32_t value) noexcept {
    at[0] = static_cast<uint8_t>(value);
    at[1] = static_cast<uint8_t>(value >> 8);
    at[2] = static_cast<uint8_t>(value >> 16);
    at[3] = static_cast<uint8_t>(value >> 24);
}

// A 24-bit BMP: a 54-byte header, then rows bottom-up with each padded to 4 bytes.
// Written by hand rather than vendoring an encoder for one debug path, and BMP rather
// than PPM because Windows opens it without asking what it is.
//
// Input: rgba is width * height * 4, top row first, red first
static bool WriteBmp(const char* path, uint32_t width, uint32_t height,
                     const uint8_t* rgba) noexcept {
    const uint32_t rowBytes = width * 3;
    const uint32_t pad = (4 - (rowBytes % 4)) % 4;
    const uint32_t imageBytes = (rowBytes + pad) * height;

    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        LOG("[capture] cannot write %s\n", path);
        return false;
    }

    uint8_t header[54]{};
    header[0] = 'B';
    header[1] = 'M';
    Put32(header + 2, 54 + imageBytes);   // file size
    Put32(header + 10, 54);               // where the pixels start
    Put32(header + 14, 40);               // DIB header size
    Put32(header + 18, width);
    Put32(header + 22, height);
    header[26] = 1;                       // planes
    header[28] = 24;                      // bits per pixel
    Put32(header + 34, imageBytes);
    std::fwrite(header, 1, sizeof(header), file);

    std::vector<uint8_t> row(rowBytes + pad, 0);
    for (uint32_t y = 0; y < height; ++y) {
        // BMP counts rows from the bottom, and stores them as B, G, R.
        const uint8_t* src = rgba + static_cast<size_t>(height - 1 - y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return true;
}

// A glTF file, flattened into the one mesh and the one item list this pass draws.
//
// Input:  path to a .gltf. Its .bin is opened from beside it
// Output: vertices and indices appended end to end, one DrawItem per primitive,
//         one entry per distinct (base colour, normal) pair the materials name,
//         which of those each item wants (UINT32_MAX means the primitive named
//         neither), and whether each item is double sided -- which the caller turns
//         into a pipeline.
//         false means nothing was appended -- the caller falls back to MakeSphere
//
// The DrawItem cannot carry the material itself: a set does not exist until the pool
// does, and the pool cannot be sized until this has counted the materials. So the
// index comes out beside the items and the caller joins the two.
//
// Every primitive keeps its own indices, numbered from its own first vertex, and the
// DrawItem carries that first vertex as vertexOffset. So the indices stay uint16 even
// though the buffer holds far more than 65535 vertices -- what has to fit in 16 bits
// is one primitive, and Sponza's largest is 23,038.
//
// Node transforms are not walked. Sponza is one node with a scale and no children, so
// the caller multiplies that in. cgltf_node_transform_world is where nesting would go.
static bool LoadGltf(const char* path,
                     std::vector<Vertex>* vertices,
                     std::vector<uint16_t>* indices,
                     std::vector<DrawItem>* items,
                     std::vector<uint32_t>* itemMaterial,
                     std::vector<MaterialSource>* materialSources) noexcept {
    cgltf_options options{};
    cgltf_data* data = nullptr;

    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
        LOG("[gltf] cannot parse %s\n", path);
        return false;
    }

    // The JSON only names the .bin; this opens it. Skipping it leaves every accessor
    // pointing at nothing, and the failure looks like an empty model rather than a
    // missing file.
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        LOG("[gltf] cannot load buffers for %s\n", path);
        cgltf_free(data);
        return false;
    }
    if (cgltf_validate(data) != cgltf_result_success) {
        LOG("[gltf] %s did not validate\n", path);
        cgltf_free(data);
        return false;
    }

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];

            // The pipeline is built for triangles. Anything else would need its own.
            if (prim.type != cgltf_primitive_type_triangles || prim.indices == nullptr) {
                continue;
            }

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            const cgltf_accessor* uv0 = nullptr;
            const cgltf_accessor* tan = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position)      { pos = at.data; }
                else if (at.type == cgltf_attribute_type_normal)   { nrm = at.data; }
                else if (at.type == cgltf_attribute_type_tangent)  { tan = at.data; }
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) {
                    uv0 = at.data;
                }
            }
            if (pos == nullptr) { continue; }

            const size_t first = vertices->size();
            if (first > 0x7FFFFFFF) {
                LOG("[gltf] more vertices than vertexOffset can address\n");
                cgltf_free(data);
                return false;
            }
            if (pos->count > 0xFFFF) {
                LOG("[gltf] primitive has %zu vertices, uint16 indices cannot reach\n",
                    (size_t)pos->count);
                cgltf_free(data);
                return false;
            }

            vertices->resize(first + pos->count);
            for (cgltf_size v = 0; v < pos->count; ++v) {
                Vertex& out = (*vertices)[first + v];
                // read_float unpacks whatever the accessor stores -- normalized bytes,
                // shorts, strided floats -- which is most of why this library is here.
                cgltf_accessor_read_float(pos, v, out.position, 3);
                if (nrm != nullptr) { cgltf_accessor_read_float(nrm, v, out.normal, 3); }
                if (uv0 != nullptr) { cgltf_accessor_read_float(uv0, v, out.uv, 2); }
                if (tan != nullptr) { cgltf_accessor_read_float(tan, v, out.tangent, 4); }
            }

            const size_t firstIndex = indices->size();
            indices->reserve(firstIndex + prim.indices->count);
            for (cgltf_size i = 0; i < prim.indices->count; ++i) {
                indices->push_back(
                    static_cast<uint16_t>(cgltf_accessor_read_index(prim.indices, i)));
            }

            // Two things the material says that land in different places: the cutoff
            // is a number the shader compares against, cull is rasterizer state. Both
            // belong to the material; only the cutoff still rides the draw, because
            // moving it means giving the material a uniform buffer.
            //
            // In this asset the two move together (all 3 MASK materials are also
            // double sided), but nothing in glTF says they must, so they are read
            // apart.
            float cutoff = 0.0f;
            bool doubleSided = false;
            if (prim.material != nullptr) {
                if (prim.material->alpha_mode == cgltf_alpha_mode_mask) {
                    cutoff = prim.material->alpha_cutoff;
                }
                doubleSided = prim.material->double_sided != 0;
            }

            // Which images this material names, as a position in a list built as we
            // go. Keyed on the pair, not on cgltf_material and not on base colour
            // alone: two materials naming the same two images should be one entry,
            // and two that share a base colour but differ in normal map must not be.
            uint32_t material = UINT32_MAX;
            if (prim.material != nullptr) {
                MaterialSource named;
                named.doubleSided = doubleSided;
                if (prim.material->has_pbr_metallic_roughness) {
                    const cgltf_texture* tex =
                        prim.material->pbr_metallic_roughness.base_color_texture.texture;
                    if (tex != nullptr && tex->image != nullptr
                            && tex->image->uri != nullptr) {
                        named.baseColor = tex->image->uri;
                    }
                }
                const cgltf_texture* nrm2 = prim.material->normal_texture.texture;
                if (nrm2 != nullptr && nrm2->image != nullptr
                        && nrm2->image->uri != nullptr) {
                    named.normal = nrm2->image->uri;
                }

                if (!named.baseColor.empty() || !named.normal.empty()) {
                    for (size_t m = 0; m < materialSources->size(); ++m) {
                        if ((*materialSources)[m].baseColor == named.baseColor
                                && (*materialSources)[m].normal == named.normal
                                && (*materialSources)[m].doubleSided == named.doubleSided) {
                            material = static_cast<uint32_t>(m);
                            break;
                        }
                    }
                    if (material == UINT32_MAX) {
                        material = static_cast<uint32_t>(materialSources->size());
                        materialSources->push_back(named);
                    }
                }
            }
            itemMaterial->push_back(material);

            DrawItem item{};
            item.alphaCutoff = cutoff;
            item.range = {static_cast<uint32_t>(firstIndex),
                          static_cast<uint32_t>(prim.indices->count)};
            item.vertexOffset = static_cast<int32_t>(first);
            items->push_back(item);
        }
    }

    LOG("[gltf] %s: %zu primitives, %zu vertices, %zu indices, %zu materials\n",
        path, items->size(), vertices->size(), indices->size(), materialSources->size());

    cgltf_free(data);
    return !items->empty();
}

// Small cells on purpose: one texel per cell makes a wrong uv obvious, and the
// LINEAR sampler softens the edges. The fallback when the scene names no texture.
//
// Output: pixels[size * size * 4], RGBA8
static void MakeChecker(uint32_t size, uint8_t* pixels) noexcept {
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint8_t v = ((x + y) % 2 == 0) ? 255 : 70;
            uint8_t* p = pixels + (y * size + x) * 4;
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    }
}

// Effect: reads an image file into a texture, ready for a set to name it
//
// 4 channels forced: the shader samples a vec4 and there is no guaranteed 8-bit
// three-channel format. stb expands whatever the file stores.
//
// The format is the caller's, and it is not a preference. Base colour is authored in
// sRGB and must say so. A normal map is a direction, not a colour: read as SRGB every
// texel is bent toward the flat normal, nothing reports it, and the picture is just
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

    const TextureDesc desc{{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                           format, VK_SAMPLE_COUNT_1_BIT,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
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
    // Declarations, in destruction order. The fill order below is different.
    // ========================================================================
    //
    // Creation and destruction cannot be a single line:
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
    Renderer       renderer;

    // Ask, then build
    // ========================================================================
    //
    //   ask     instance and surface are inputs to the questions, not results
    //   build   device -> commands and descriptors -> what needs them
    //
    // Command buffers divide by when they run, descriptor sets by what they name.

    // glfwInit is first only because windowSystem is declared first and so dies last.
    if (!InitWindowSystem(&windowSystem)) { return 1; }
    if (!CreateInstance(&inst)) { return 1; }
    if (!OpenWindow(inst, kWindowWidth, kWindowHeight, "Lambda Engine", &window)) {
        return 1;
    }

    // What the hardware is asked: which GPU, and which of its queue families. Nothing
    // about what we intend to draw -- that is the next question and ours to answer.
    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    // Two questions the picked GPU answers alone -- no device, nothing to destroy.
    // Formats, not images: the pipelines below need the answer, and securing a place
    // to draw happens again on every resize, which makes it the loop's business.
    AttachmentFormats formats;
    if (!ChooseAttachmentFormats(inst, selection.gpu, &formats)) { return 1; }
    if (!SelectSurfaceFormat(inst, selection.gpu, &window)) { return 1; }

    // selection is absorbed here and not kept -- nothing below this line reads it.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }
    if (!CreateCommands(dev, &commands)) { return 1; }

    // Here, not with the passes below: the descriptor pool has to be told about this
    // one's set before it is created, and the font it points at is uploaded in here.
    if (!CreateGui(dev, commands, window, &renderer.guiPass)) { return 1; }

    // Passes
    // ------------------------------------------------------------------------
    //
    // One for the scene pass, one for the post pass -- not a rule about passes, just
    // what this scene asks for. It was two until cull became dynamic state: the second
    // differed in that one field, which is a register and not a shader.
    //
    // viewportY stays baked. It decides frontFace, and both would have to move
    // together -- nothing here asks for that.
    GraphicsPipelineDesc opaqueDesc;
    opaqueDesc.vertPath = "Shaders/mesh.vert.spv";
    opaqueDesc.fragPath = "Shaders/mesh.frag.spv";
    opaqueDesc.vertexInput = &VertexInput();
    opaqueDesc.formats = formats;
    opaqueDesc.viewportY = ViewportY::Up;            // our world is y-up
    opaqueDesc.polygonMode = VK_POLYGON_MODE_FILL;
    opaqueDesc.blending = Blending::Opaque;
    if (!CreateGraphicsPipeline(dev, opaqueDesc, &renderer.scenePipeline)) { return 1; }

    // The format was settled by SelectSurfaceFormat above and does not change, so this
    // pipeline is right from the start and nothing rebuilds it.
    //
    // No vertex input, no depth, 1 sample -- MSAA ended at the resolve.
    GraphicsPipelineDesc presentDesc;
    presentDesc.vertPath = "Shaders/fullscreen.vert.spv";
    presentDesc.fragPath = "Shaders/fullscreen.frag.spv";
    presentDesc.formats = AttachmentFormats{window.surfaceFormat.format};
    presentDesc.viewportY = ViewportY::Down;   // the shader makes its own uv
    if (!CreateGraphicsPipeline(dev, presentDesc, &renderer.presentPipeline)) { return 1; }

    // The panel. A different vertex type, a different set layout, and the only one
    // of the three that blends -- a window has to be see-through to be over anything.
    //
    // Same target as present, so the same format and 1 sample. y-down because ImGui
    // works in window pixels with the origin at the top left.
    GraphicsPipelineDesc guiDesc;
    guiDesc.vertPath = "Shaders/gui.vert.spv";
    guiDesc.fragPath = "Shaders/gui.frag.spv";
    guiDesc.vertexInput = &GuiVertexInput();
    guiDesc.formats = AttachmentFormats{window.surfaceFormat.format};
    guiDesc.viewportY = ViewportY::Down;
    guiDesc.blending = Blending::Translucent;
    if (!CreateGraphicsPipeline(dev, guiDesc, &renderer.guiPipeline)) { return 1; }

    // Render resolution
    // ------------------------------------------------------------------------
    //
    // The other half of what a render target looks like; formats is the first.
    // Constant, so aspect cannot change while the targets live.
    constexpr VkExtent2D kRenderExtent{kRenderWidth, kRenderHeight};
    const float aspect = static_cast<float>(kRenderExtent.width)
                       / static_cast<float>(kRenderExtent.height);

    // No proj[1][1] *= -1: the viewport height is already negative.
    // Depth lands in [0,1] thanks to GLM_FORCE_DEPTH_ZERO_TO_ONE on the CMake target.
    const glm::mat4 proj =
        glm::perspective(glm::radians(kFovDegrees), aspect, kNearPlane, kFarPlane);

    // Scene
    // ------------------------------------------------------------------------
    //
    // Heap, not the stack: Sponza is 192,496 vertices, which is 8.8 MB as Vertex[48].
    // Init may allocate freely (see the table at the top of this file); the frame loop
    // still may not, and nothing below the loop touches these again after the upload.
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<DrawItem> items;

    // The asset is gitignored, so a machine without it is normal. The generated sphere
    // is the fallback, and it is also what says whether a blank screen is the loader's
    // fault or the renderer's.
    const char* const kScenePath = LAMBDA_ASSET_ROOT "/Sponza/Sponza.gltf";
    std::vector<uint32_t> itemMaterial;      // one per item, indexing materialSources
    std::vector<MaterialSource> materialSources;
    bool loaded = false;
    if (std::FILE* probe = std::fopen(kScenePath, "rb")) {
        std::fclose(probe);
        loaded = LoadGltf(kScenePath, &vertices, &indices, &items,
                          &itemMaterial, &materialSources);
    } else {
        LOG("[scene] no %s -- drawing the generated sphere instead\n", kScenePath);
    }

    // glTF gives Sponza in centimetres and puts the scale on its one node. Applied
    // here rather than baked into the positions so the file stays the source of truth.
    constexpr float kSponzaScale = 0.008f;
    if (loaded) {
        const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3{kSponzaScale});
        for (DrawItem& item : items) { item.model = model; }
    }

    if (!loaded) {
        constexpr uint32_t kStacks = 16;
        constexpr uint32_t kSlices = 32;
        constexpr float kRadius = 0.7f;
        constexpr uint32_t kVertexCount = (kStacks + 1) * (kSlices + 1);
        constexpr uint32_t kIndexCount = kStacks * kSlices * 6;
        static_assert(kVertexCount <= 0xFFFF, "index type is uint16");

        vertices.resize(kVertexCount);
        indices.resize(kIndexCount);
        MakeSphere(kStacks, kSlices, kRadius, vertices.data(), indices.data());

        constexpr IndexRange kSphereIndices{0, kIndexCount};
        static_assert(kSphereIndices.End() == kIndexCount,
                      "spans do not cover the index array");

        // Five of them, so depth and the lighting have something to work on. One mesh
        // and one span for all five -- only the matrix differs, which is what a
        // DrawItem is for.
        const glm::mat4 half = glm::scale(glm::mat4(1.0f), glm::vec3{0.5f});
        const glm::mat4 kPlacements[] = {
            glm::mat4(1.0f),
            glm::translate(glm::mat4(1.0f), glm::vec3{-1.5f, 0.0f, 0.0f}) * half,
            glm::translate(glm::mat4(1.0f), glm::vec3{ 1.5f, 0.0f, 0.0f}) * half,
            glm::translate(glm::mat4(1.0f), glm::vec3{ 0.0f, 1.1f, -1.2f}) * half,
            glm::translate(glm::mat4(1.0f), glm::vec3{ 0.0f, -1.0f, 0.9f}) * half,
        };
        // No material of their own: they take the checker, like a glTF primitive
        // that names no texture.
        // No material of their own: they take the checker, like a glTF primitive
        // that names no texture. Closed shapes, so they cull like the opaque ones.
        for (const glm::mat4& m : kPlacements) {
            // material is filled in below, once the Material array exists.
            items.push_back(DrawItem{m, 1.0f, 0.0f, kSphereIndices, nullptr, 0});
            itemMaterial.push_back(UINT32_MAX);
        }
    }

    // stride is the one thing a mesh can say about its vertices; the pipeline says
    // which bytes are what.
    const MeshDesc meshDesc{sizeof(Vertex),
                            static_cast<uint32_t>(vertices.size()),
                            static_cast<uint32_t>(indices.size())};
    if (!CreateMesh(dev, commands, meshDesc, vertices.data(), indices.data(),
                    &renderer.mesh)) {
        return 1;
    }

    // Textures
    // ------------------------------------------------------------------------
    //
    // One per image the scene named, and the checker last. The checker is not a
    // leftover: a primitive can name no texture, and the sphere fallback names none
    // at all, so every item still needs something to point at.
    //
    // URIs are relative to the .gltf, per the spec, and Sponza keeps its images
    // beside it.
    // Two per material, laid out in pairs, plus one extra material at the end for an
    // item that named neither -- a primitive with no textures, or the sphere fallback.
    //
    // A material missing one gets a stand-in rather than a null. The set has two
    // bindings and every one of them has to point somewhere; a null would be a
    // validation error at bind time, and a branch in the shader would be a third way
    // to say the same thing.
    const uint32_t materialCount = static_cast<uint32_t>(materialSources.size()) + 1;
    renderer.textures.resize(static_cast<size_t>(materialCount) * 2);

    // Code, not a file, so a machine without the asset still draws something that
    // shows whether uv and the sampler are right.
    //
    // SRGB: this is multiplied with the shader's output, so it must be in the same
    // space as the render target. UNORM here would brighten the result.
    constexpr uint32_t kCheckerSize = 8;
    uint8_t checkerPixels[kCheckerSize * kCheckerSize * 4]{};
    MakeChecker(kCheckerSize, checkerPixels);
    const TextureDesc checkerDesc{{kCheckerSize, kCheckerSize},
                                  VK_FORMAT_R8G8B8A8_SRGB, VK_SAMPLE_COUNT_1_BIT,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                      | VK_IMAGE_USAGE_SAMPLED_BIT};

    // One texel, and the opposite space from the checker: (128,128,255) decodes to
    // +z, which is the geometric normal unchanged. UNORM for the same reason the
    // loaded ones are -- this is a direction.
    const uint8_t flatNormalPixels[4]{128, 128, 255, 255};
    const TextureDesc flatNormalDesc{{1, 1},
                                     VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                         | VK_IMAGE_USAGE_SAMPLED_BIT};

    for (uint32_t i = 0; i < materialCount; ++i) {
        const MaterialSource named =
            i < materialSources.size() ? materialSources[i] : MaterialSource{};
        Texture& base = renderer.textures[static_cast<size_t>(i) * 2];
        Texture& normal = renderer.textures[static_cast<size_t>(i) * 2 + 1];

        if (!named.baseColor.empty()) {
            const std::string path = std::string(LAMBDA_ASSET_ROOT "/Sponza/")
                                   + named.baseColor;
            if (!LoadTextureFile(dev, commands, path.c_str(),
                                 VK_FORMAT_R8G8B8A8_SRGB, &base)) { return 1; }
        } else if (!CreateTextureFromPixels(dev, commands, checkerDesc, checkerPixels,
                                            sizeof(checkerPixels), &base)) {
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
    }

    // Descriptors
    // ------------------------------------------------------------------------
    //
    // Here, not up with the pipelines, because the pool cannot be sized until the
    // scene has been counted. That is what a material being data means: the number
    // of sets is no longer something this file knows in advance.
    //
    // Three claims, and the counts come from three different places -- frames in
    // flight for the two frame sets, materials for the material set. How many
    // descriptors that is per set is not asked here: CreateDescriptors reads it off
    // the layout, so the second binding a material grew did not reach this line.
    const SetRequest setRequests[] = {
        {&renderer.scenePipeline.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer.scenePipeline.setLayouts[kMaterialSet], materialCount},
        {&renderer.presentPipeline.setLayouts[kFrameSet], kFramesInFlight},
        // One, and counted by neither of the other two reasons: there is one font.
        {&renderer.guiPipeline.setLayouts[0], 1},
    };
    if (!CreateDescriptors(dev, setRequests,
                           static_cast<uint32_t>(std::size(setRequests)),
                           &renderer.descriptors)) { return 1; }

    // The pairs, as the two pointers a set is filled from. Built here rather than
    // stored, because textures owns them and this is only a way of reading it.
    std::vector<MaterialDesc> sources(materialCount);
    for (uint32_t i = 0; i < materialCount; ++i) {
        // The stand-in is last and the loader did not describe it: closed shapes, so
        // it culls like the opaque ones.
        const bool doubleSided = i < materialSources.size() && materialSources[i].doubleSided;
        // The cast is for the braced init: the enum is int, the flags field is
        // unsigned, and that counts as narrowing here.
        sources[i] = {&renderer.textures[static_cast<size_t>(i) * 2],
                      &renderer.textures[static_cast<size_t>(i) * 2 + 1],
                      static_cast<VkCullModeFlags>(doubleSided ? VK_CULL_MODE_NONE
                                                               : VK_CULL_MODE_BACK_BIT)};
    }

    if (!CreateGuiSet(renderer.descriptors, renderer.guiPipeline,
                      &renderer.guiPass)) { return 1; }

    renderer.materials.resize(materialCount);
    if (!CreateMaterials(renderer.descriptors, renderer.scenePipeline, sources.data(),
                         materialCount, renderer.materials.data())) { return 1; }

    // Join the two halves the loader had to hand back separately. The stand-in pair is
    // last, so it is what UINT32_MAX resolves to.
    // Contract: renderer.materials must not be resized after this -- the items point
    //           into it. It is filled once above and never grows.
    const uint32_t kNoTexture = materialCount - 1;
    for (size_t i = 0; i < items.size(); ++i) {
        const uint32_t index = itemMaterial[i];
        items[i].material =
            &renderer.materials[index == UINT32_MAX ? kNoTexture : index];
    }

    // The draw order, now that both things a bind depends on hang off one pointer.
    //
    // Only grouping matters, not which group comes first: a bind happens where two
    // neighbours differ, so any total order that puts equal materials together reaches
    // the same count. That is why comparing the pointers is enough -- they are
    // positions in one vector, and their order carries no meaning beyond "not equal".
    //
    // Cull first, even though it is a function of the material and so adds no
    // information. It adds an ordering: sorting on the material alone leaves the cull
    // groups interleaved, and grouping by the coarser value first costs nothing,
    // because a material never straddles two cull modes.
    //
    // Stable, so items that tie keep the order the file gave them. Nothing depends on
    // it yet -- blending is off, so no draw has to come after another.
    std::stable_sort(items.begin(), items.end(),
                     [](const DrawItem& a, const DrawItem& b) noexcept {
                         if (a.material->cullMode != b.material->cullMode) {
                             return a.material->cullMode < b.material->cullMode;
                         }
                         return a.material < b.material;
                     });

    // Frames
    // ------------------------------------------------------------------------
    //
    // Passes first, in dependency order: the post pass's sets name what the scene
    // pass made. A slot owns none of that -- it only knows which frame it is.
    if (!CreateScenePass(dev, renderer.descriptors, formats, kRenderExtent,
                         renderer.mesh, renderer.scenePipeline,
                         &renderer.scenePass)) { return 1; }
    if (!CreatePostProcessPass(renderer.descriptors, renderer.scenePass,
                               renderer.presentPipeline, &renderer.postPass)) {
        return 1;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateFrameSlot(dev, commands, i, &renderer.slots[i])) { return 1; }
    }


    LOG("close the window to exit. The panel switches features off.\n");

    // Frame state
    // ------------------------------------------------------------------------
    //
    // Everything the loop carries across frames. Not a struct: only lookAt reads the
    // camera values, so grouping would enforce nothing. Split them once something
    // else reads them -- frustum culling, a light's viewpoint, a shadow pass.
    uint32_t slotIndex = 0;       // which slot this frame borrows
    double lastTime = glfwGetTime();

    // What the item order costs in state changes, reported once. Here rather than in
    // the panel because it is a number to compare between runs, not to watch.
    bool loggedDrawStats = false;

    // Set it to a path and the first frame is written there and the program exits.
    // An environment variable for the reason LAMBDA_FIXED_TIME is one: what a capture
    // wants and what a person running this wants are opposite.
    //
    // Implies fixed time -- a capture of a moving light is not comparable to anything.
    const char* const capturePath = std::getenv("LAMBDA_CAPTURE");

    // Deterministic capture.
    //
    // The light is the only thing here that reads absolute time, and it turns every
    // frame -- so two runs never draw the same picture, and two screenshots cannot be
    // told apart from what the code changed. With LAMBDA_FIXED_TIME set, it stops.
    //
    // An environment variable rather than a Config constant: what a capture wants and
    // what a person running it wants are opposite, and a constant would have to be
    // edited between them.
    //
    // Read once. getenv per frame would be a lookup for a value that cannot change.
    // dt still comes from the real clock, or the camera would stop answering keys.
    const bool fixedTime =
        std::getenv("LAMBDA_FIXED_TIME") != nullptr || capturePath != nullptr;
    constexpr float kFixedTime = 1.0f;   // any constant. 1.0 puts the light off-axis

    // What to leave out. Edited by the panel's checkboxes, read by the uniform.
    //
    // Comparing a feature against its own absence used to mean checking out the
    // commit before it. Now it is one click, with the same camera and the same light
    // on the same screen -- which is the only way the difference is honest.
    //
    // Not in Config.h: a constant would have to be edited and rebuilt, and the point
    // is to see both within a second of each other.
    ViewOptions viewOptions;   // 'view' is the matrix below

    glm::vec3 eye{0.0f, 0.0f, 3.5f};
    float yaw = -90.0f;           // -90 looks down -z, per the forward expression below
    float pitch = 0.0f;

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // Sleep until an event arrives while minimized.
        //
        // Reset the clock after waking: the sleep is not a frame, and counting it
        // would make the next dt jump and teleport the camera.
        if (!WindowHasDrawableSize(window)) {
            glfwWaitEvents();
            lastTime = glfwGetTime();
            continue;
        }

        // What to draw
        // --------------------------------------------------------------------
        //
        // Nothing here touches the GPU, so it could run while minimized. What comes
        // out is state -- camera, light, items -- and the next section sends it.

        // Clock
        //
        // One clock reading, two values: t is absolute (object spin), dt is the gap
        // (camera movement). Reading twice would let them drift apart.
        const double now = glfwGetTime();
        const float t = fixedTime ? kFixedTime : static_cast<float>(now);
        const float dt = static_cast<float>(now - lastTime);
        lastTime = now;

        // Input -> camera
        //
        // --------------------------------------------------------------------
        //
        // glfwGetKey polls the state glfwPollEvents cached, so this block reads the same
        // value wherever it sits. A callback suits an event; holding a key is a state.
        //
        // Speeds are multiplied by dt, or the frame rate becomes the speed.

        const auto held = [&](int key) {
            return glfwGetKey(window.handle, key) == GLFW_PRESS;
        };

        if (held(GLFW_KEY_LEFT))  { yaw   -= kTurnSpeed * dt; }
        if (held(GLFW_KEY_RIGHT)) { yaw   += kTurnSpeed * dt; }
        if (held(GLFW_KEY_UP))    { pitch += kTurnSpeed * dt; }
        if (held(GLFW_KEY_DOWN))  { pitch -= kTurnSpeed * dt; }

        // At +-90 forward aligns with world up and the cross product below collapses.
        pitch = glm::clamp(pitch, -89.0f, 89.0f);

        const glm::vec3 forward = glm::normalize(glm::vec3{
            glm::cos(glm::radians(yaw)) * glm::cos(glm::radians(pitch)),
            glm::sin(glm::radians(pitch)),
            glm::sin(glm::radians(yaw)) * glm::cos(glm::radians(pitch)),
        });

        // Derived from forward, so it cannot drift out of step with it.
        constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
        const glm::vec3 right = glm::normalize(glm::cross(forward, kWorldUp));

        if (held(GLFW_KEY_W)) { eye += forward * kMoveSpeed * dt; }
        if (held(GLFW_KEY_S)) { eye -= forward * kMoveSpeed * dt; }
        if (held(GLFW_KEY_D)) { eye += right   * kMoveSpeed * dt; }
        if (held(GLFW_KEY_A)) { eye -= right   * kMoveSpeed * dt; }
        if (held(GLFW_KEY_E)) { eye += kWorldUp * kMoveSpeed * dt; }
        if (held(GLFW_KEY_Q)) { eye -= kWorldUp * kMoveSpeed * dt; }

        // center is eye + forward. An absolute target would pin the gaze to one point
        // and rotation would stop working.
        const glm::mat4 view = glm::lookAt(eye, eye + forward, kWorldUp);
        const glm::mat4 camera = proj * view;

        // Light
        //
        // One directional light, circling so the brightness visibly changes -- the
        // objects turn about z, which leaves their normals fixed.
        const glm::vec3 lightDir = glm::normalize(
            glm::vec3{std::cos(t) * 0.7f, 0.5f, std::sin(t) * 0.7f});

        // Fill this frame's share of the pass
        //
        // Assignment only, so it belongs up here: what reaches the GPU, and when, is
        // RecordFrame's. The camera and light go to the pass because every draw in it
        // reads them. The item list is handed in as an argument instead -- it is this
        // frame's alone and no pass owns it.
        //
        // Through slot.index, not slotIndex: recording picks the pass's frame that way
        // too, and one of the two would otherwise have to be kept in step by hand.
        FrameSlot& slot = renderer.slots[slotIndex];
        renderer.scenePass.frames[slot.index].uniformValue =
            {camera, glm::vec4{lightDir, 0.0f},
             glm::vec4{1.0f, 0.95f, 0.9f, 0.15f}, glm::vec4{eye, 48.0f},
             viewOptions.normalMap ? 1.0f : 0.0f, viewOptions.baseColor ? 1.0f : 0.0f,
             viewOptions.specular ? 1.0f : 0.0f, viewOptions.alphaMask ? 1.0f : 0.0f};

        // Draw it
        // --------------------------------------------------------------------

        // Where this frame goes. Lives until present and no further, and the loop
        // only carries it -- BeginFrame is what pairs it with this slot.
        FrameTarget target;
        const FrameResult begun = BeginFrame(dev, &window, slot, &target);
        if (begun == FrameResult::Fatal) { break; }

        if (begun == FrameResult::Skip) { continue; }

        // Everything from here breaks instead of continuing. The acquire already
        // happened, and skipping the submit would leave a signalled semaphore and a
        // reset fence with nobody left to wait on them.

        // The panel, after the acquire because it reports the image this frame got.
        // Nothing here touches the GPU -- it only fills a draw list that
        // RecordGuiPass reads. Below the Skip return on purpose: a frame that is not
        // recorded would leave that list stale.
        GuiFrameInfo guiInfo;
        guiInfo.frameSeconds = dt;
        guiInfo.drawCount = static_cast<uint32_t>(items.size());
        guiInfo.materialCount = materialCount;
        guiInfo.descriptors = &renderer.descriptors;
        guiInfo.scenePipeline = &renderer.scenePipeline;
        guiInfo.presentPipeline = &renderer.presentPipeline;
        guiInfo.uniformBytes = static_cast<uint32_t>(sizeof(SceneUniform));
        guiInfo.pushBytes = static_cast<uint32_t>(sizeof(PushConstants));
        guiInfo.vertexStride = static_cast<uint32_t>(sizeof(Vertex));
        guiInfo.vertexAttributes = VertexInput().vertexAttributeDescriptionCount;
        guiInfo.framesInFlight = kFramesInFlight;
        guiInfo.mesh = &renderer.mesh;
        guiInfo.guiPipeline = &renderer.guiPipeline;
        guiInfo.slotIndex = slot.index;
        guiInfo.sceneColor = &renderer.scenePass.frames[slot.index].color;
        guiInfo.sceneResolve = &renderer.scenePass.frames[slot.index].colorResolve;
        guiInfo.sceneDepth = &renderer.scenePass.frames[slot.index].depth;
        guiInfo.frameTarget = target.texture;
        BuildGui(&viewOptions, guiInfo);


        // Only the texture: recording has no use for the rest of the target.
        DrawStats drawStats;
        if (!RecordFrame(slot, renderer.scenePass, renderer.postPass,
                         renderer.guiPass, *target.texture,
                         items.data(), static_cast<uint32_t>(items.size()),
                         &drawStats)) {
            break;
        }

        // Once. The list does not change between frames, so neither do these -- and a
        // line per frame would bury the one number that matters.
        if (!loggedDrawStats) {
            LOG("[draw] %u draws, %u material binds, %u cull changes\n",
                drawStats.draws, drawStats.materialBinds, drawStats.cullChanges);
            loggedDrawStats = true;
        }

        // Frame.h holds the reason submit and present are separate.
        if (!SubmitFrame(dev, slot, target)) {
            break;
        }
        if (!PresentFrame(dev, &window, target)) {
            break;
        }

        // One frame, then out. Everything the picture depends on is settled before the
        // loop -- textures uploaded, camera at its start -- so waiting longer only adds
        // whatever the clock and the keyboard did meanwhile.
        //
        // The wait is for this frame's own submit: colorResolve is being written by
        // the commands just sent, and ReadTexturePixels copies from it.
        if (capturePath != nullptr) {
            dev.table.vkDeviceWaitIdle(dev.handle);
            const Texture& shot = renderer.scenePass.frames[slot.index].colorResolve;
            std::vector<uint8_t> pixels;
            if (ReadTexturePixels(dev, commands, shot,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &pixels)
                    && WriteBmp(capturePath, shot.desc.extent.width,
                                shot.desc.extent.height, pixels.data())) {
                LOG("[capture] %ux%u -> %s\n",
                    shot.desc.extent.width, shot.desc.extent.height, capturePath);
            }
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
