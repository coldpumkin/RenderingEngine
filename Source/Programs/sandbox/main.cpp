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
//
// The sections below, in order. It is also the order things depend on each other, so
// nothing here can be moved up past what it reads.
//
//   file scope    Scene data     the loader, and the plain arrays it hands back
//                 Capture        one frame to a file, for comparing two builds
//
//   main, once    Declarations   in destruction order, which is not the fill order
//                 Ask            what the GPU and the surface answer before anything
//                 Targets        what we render into, and the lens each one answers to
//                 Passes         programs, then a pipeline per variant
//                 Scene          the mesh and the draw list
//                 Textures       the images the materials name
//                 Descriptors    the pool, sized by what the scene turned out to be
//                 Frames         per-frame values, then the passes, then the slots
//                 Frame state    what the loop carries across frames
//
//   main, loop    What to draw   clock, camera, light. Touches no GPU call
//                 Draw it        acquire, resize if asked, record, submit, present


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
#include <cstdio>     // fopen, for the capture's own file
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
// CreateMesh and CreateTextureFromPixels take.
//
// Nothing here writes a Vertex by hand, and the loader is the only thing that fills
// the array. Vertex answers to scene.vert -- the shader declares the locations,
// spirv-reflect reports them, CheckVertexInterface compares the two -- and that check
// sees whether a field is supplied, never whether it holds the right numbers. A
// hand-written tangent is trigonometry nothing can verify; a loaded one is a field in
// the file, and a shader that asks for one more is one more accessor to read.

// What the loader found for one material, and the key two of them are compared on.
//
// An empty path means the material named no image there, and the caller substitutes a
// neutral one -- white for the base colour, flat for the normal. Neutral, not
// distinctive: glTF says a material without a baseColorTexture is its factor alone,
// and white is the texture that multiplies to exactly that.
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
    std::string metallicRoughness;
    bool doubleSided = false;

    // The numbers, in the key for the reason doubleSided is: two glTF materials naming
    // the same images but multiplying them differently are two materials. Every one of
    // Sponza's 25 sets baseColorFactor, so leaving it out was throwing away the only
    // thing that distinguishes some of them.
    MaterialParams params;
};

// Capture
// ============================================================================
//
// What the scene pass produced, as a file, read off the GPU. The alternative was a
// screenshot of the window, which reads whatever is at those coordinates -- the same
// binary measured 0.95% and 14.26% black on two runs. This reads the image itself, so
// two runs of one build are identical by construction and a diff is only ever code.
//
// colorResolve is the subject: whatever size the render targets currently are, and
// before the panel is drawn on top. With the target following the window that size is
// the window's, so a capture is only comparable against another taken the same way.

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

// Tangents for one primitive, from its positions and uvs.
//
// glTF makes TANGENT optional and says a runtime must generate it when a normal
// texture is present without one. Sponza has exactly one primitive without it, and
// that material names no normal map -- so nothing the shader builds from this reaches
// the picture today. It is generated anyway, because zero is not a neutral value: the
// TBN's first column would be normalize(0), which is NaN, and the day that material
// gets a normal map the NaN is what would show.
//
// The standard construction. Across one triangle the surface is a plane, so uv is an
// affine function of position and the tangent is the direction u grows in:
//
//   [du1]   [dp1]        T = (dp1 * dv2 - dp2 * dv1) / (du1 * dv2 - du2 * dv1)
//   [du2]   [dp2]
//
// Accumulated per vertex and normalized after, which is what averages the seams
// between triangles. Not MikkTSpace -- that splits vertices to keep mirrored uvs
// exact, and we have one primitive to serve.
//
// Contract: normals are already written, and w is the bitangent sign the fragment
//           stage multiplies cross(N, T) by.
static void GenerateTangents(Vertex* vertices, size_t vertexCount,
                             const uint16_t* indices, size_t indexCount) noexcept {
    std::vector<glm::vec3> accumulated(vertexCount, glm::vec3{0.0f});

    for (size_t i = 0; i + 2 < indexCount; i += 3) {
        const uint16_t i0 = indices[i];
        const uint16_t i1 = indices[i + 1];
        const uint16_t i2 = indices[i + 2];

        const Vertex& v0 = vertices[i0];
        const Vertex& v1 = vertices[i1];
        const Vertex& v2 = vertices[i2];

        const glm::vec3 p0{v0.position[0], v0.position[1], v0.position[2]};
        const glm::vec3 dp1 = glm::vec3{v1.position[0], v1.position[1], v1.position[2]} - p0;
        const glm::vec3 dp2 = glm::vec3{v2.position[0], v2.position[1], v2.position[2]} - p0;

        const float du1 = v1.uv[0] - v0.uv[0];
        const float dv1 = v1.uv[1] - v0.uv[1];
        const float du2 = v2.uv[0] - v0.uv[0];
        const float dv2 = v2.uv[1] - v0.uv[1];

        // Degenerate in uv: the triangle covers no area in the texture, so it says
        // nothing about which way u runs. Skipped rather than divided by.
        const float determinant = du1 * dv2 - du2 * dv1;
        if (std::fabs(determinant) < 1e-12f) { continue; }

        const glm::vec3 tangent = (dp1 * dv2 - dp2 * dv1) / determinant;
        accumulated[i0] += tangent;
        accumulated[i1] += tangent;
        accumulated[i2] += tangent;
    }

    for (size_t v = 0; v < vertexCount; ++v) {
        Vertex& out = vertices[v];
        const glm::vec3 normal{out.normal[0], out.normal[1], out.normal[2]};
        glm::vec3 tangent = accumulated[v];

        // A vertex no triangle contributed to, or one whose triangles were all
        // degenerate. Any direction perpendicular to the normal is as good as another
        // when uv says nothing, and the point is only to stay off zero.
        if (glm::dot(tangent, tangent) < 1e-16f) {
            const glm::vec3 axis = std::fabs(normal.x) < 0.9f ? glm::vec3{1.0f, 0.0f, 0.0f}
                                                             : glm::vec3{0.0f, 1.0f, 0.0f};
            tangent = glm::cross(normal, axis);
        }

        // Gram-Schmidt, the same step scene.frag repeats after interpolation.
        tangent = tangent - normal * glm::dot(normal, tangent);
        if (glm::dot(tangent, tangent) < 1e-16f) { tangent = glm::vec3{1.0f, 0.0f, 0.0f}; }
        tangent = glm::normalize(tangent);

        out.tangent[0] = tangent.x;
        out.tangent[1] = tangent.y;
        out.tangent[2] = tangent.z;

        // +1 because this construction puts the bitangent at cross(N, T) already.
        // A mirrored uv island would want -1, which is what MikkTSpace tracks and
        // this does not.
        out.tangent[3] = 1.0f;
    }
}

// A glTF file, flattened into the one mesh and the one item list this pass draws.
//
// Input:  path to a .gltf. Its .bin is opened from beside it
// Output: vertices and indices appended end to end, one DrawItem per primitive,
//         one entry per distinct material the file names, and which of those each
//         item wants. Every item gets a real index: a primitive naming no material is
//         refused above.
//         false means the file could not be turned into a scene, and says in the log
//         which way. What was appended before that point is undefined: the caller
//         exits, because there is nothing else here to draw.
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

    // Two failures worth telling apart in the log, and cgltf already tells them
    // apart: the asset is gitignored, so a machine that never had it is the ordinary
    // case, and a file that is there and unreadable is not.
    const cgltf_result parsed = cgltf_parse_file(&options, path, &data);
    if (parsed != cgltf_result_success) {
        LOG(parsed == cgltf_result_file_not_found ? "[gltf] no %s\n"
                                                  : "[gltf] cannot parse %s\n",
            path);
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

            // The third link in a chain that had two. CheckVertexInterface compares
            // the .spv against the layout and SameVertexLayout compares the layout
            // against the mesh; nothing compared the asset against either, so a file
            // missing an attribute filled it with the zeroes resize left behind, and
            // a zero normal is not dark -- normalize() of it is NaN.
            //
            // Refused rather than guessed. Sponza has neither missing, so anything
            // written here to cope would be code that never runs.
            const char* missing = nullptr;
            if (nrm == nullptr)      { missing = "NORMAL"; }
            else if (uv0 == nullptr) { missing = "TEXCOORD_0"; }
            if (missing != nullptr) {
                LOG("[gltf] a primitive has no %s, and scene.vert reads it\n", missing);
                cgltf_free(data);
                return false;
            }

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
                cgltf_accessor_read_float(nrm, v, out.normal, 3);
                cgltf_accessor_read_float(uv0, v, out.uv, 2);
                if (tan != nullptr) { cgltf_accessor_read_float(tan, v, out.tangent, 4); }
            }

            const size_t firstIndex = indices->size();
            indices->reserve(firstIndex + prim.indices->count);
            for (cgltf_size i = 0; i < prim.indices->count; ++i) {
                indices->push_back(
                    static_cast<uint16_t>(cgltf_accessor_read_index(prim.indices, i)));
            }

            // After the indices, because the triangles are what tangents come from.
            // Sponza needs this for one primitive out of 103.
            if (tan == nullptr) {
                GenerateTangents(vertices->data() + first, pos->count,
                                 indices->data() + firstIndex, prim.indices->count);
            }

            // Two things the material says that land in different places: the cutoff
            // is a number the shader compares against, cull is rasterizer state. Both
            // belong to the material; only the cutoff still rides the draw, because
            // moving it means giving the material a uniform buffer.
            //
            // In this asset the two move together (all 3 MASK materials are also
            // double sided), but nothing in glTF says they must, so they are read
            // apart.
            MaterialParams params;
            bool doubleSided = false;
            if (prim.material != nullptr) {
                if (prim.material->alpha_mode == cgltf_alpha_mode_mask) {
                    params.alphaCutoff = prim.material->alpha_cutoff;
                }
                if (prim.material->has_pbr_metallic_roughness) {
                    const cgltf_pbr_metallic_roughness& pbr =
                        prim.material->pbr_metallic_roughness;
                    const cgltf_float* f = pbr.base_color_factor;
                    params.baseColorFactor = glm::vec4{f[0], f[1], f[2], f[3]};
                    params.metallic = pbr.metallic_factor;
                    params.roughness = pbr.roughness_factor;
                }
                doubleSided = prim.material->double_sided != 0;
            }

            // Which images this material names, as a position in a list built as we
            // go. Keyed on the pair, not on cgltf_material and not on base colour
            // alone: two materials naming the same two images should be one entry,
            // and two that share a base colour but differ in normal map must not be.
            // Every primitive names one. A primitive without a material is legal glTF
            // -- the spec says to draw it with the default material -- and that
            // default is a thing we would have to build and never draw with, since
            // Sponza has none. Refused instead, the way a missing NORMAL is.
            if (prim.material == nullptr) {
                LOG("[gltf] a primitive names no material\n");
                cgltf_free(data);
                return false;
            }

            uint32_t material = UINT32_MAX;
            {
                MaterialSource named;
                named.doubleSided = doubleSided;
                named.params = params;
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
                if (prim.material->has_pbr_metallic_roughness) {
                    const cgltf_texture* mr = prim.material->pbr_metallic_roughness
                                                  .metallic_roughness_texture.texture;
                    if (mr != nullptr && mr->image != nullptr
                            && mr->image->uri != nullptr) {
                        named.metallicRoughness = mr->image->uri;
                    }
                }

                // Every material the file names gets an entry, images or not. The
                // test that used to be here -- at least one texture named -- sent a
                // material with only a base colour factor to the stand-in, which
                // carries a white factor, no cutoff and back-face culling. A glTF
                // material with no images and a coloured factor is ordinary, and its
                // colour, cutoff and double_sided were read three lines above and then
                // dropped.
                //
                // What is left for the stand-in is a primitive that names no material
                // at all, which is the one case with nothing to carry.
                {
                    for (size_t m = 0; m < materialSources->size(); ++m) {
                        const MaterialSource& seen = (*materialSources)[m];
                        if (seen.baseColor == named.baseColor
                                && seen.normal == named.normal
                                && seen.metallicRoughness == named.metallicRoughness
                                && seen.doubleSided == named.doubleSided
                                && seen.params.baseColorFactor
                                       == named.params.baseColorFactor
                                && seen.params.alphaCutoff == named.params.alphaCutoff
                                && seen.params.metallic == named.params.metallic
                                && seen.params.roughness == named.params.roughness) {
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
    Renderer       renderer;

    // Ask -- what the GPU and the surface answer before anything is built
    // ========================================================================
    //
    // The instance and the surface are inputs to these questions, not results of them.
    // Everything after answers to what comes back here.

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
    // Formats, not images: images are made again on every resize.
    //
    // The display first. Its format must be sRGB, the encode nobody else in the chain
    // performs; the offscreen colour does not follow from it (Attachments.cpp).
    if (!SelectSurfaceFormat(inst, selection.gpu, &window)) { return 1; }

    // Targets -- what we render into, and the lens each one answers to
    // ------------------------------------------------------------------------
    //
    // Together because the aspect joins them: it comes out of an extent and goes into
    // a projection.

    // The render chain's colour, not the scene pass's -- every target in the chain is
    // made of it. R8G8B8A8 because WriteBmp reads red first; SRGB so blending and the
    // MSAA resolve happen in linear space. Not the swapchain's: only the swapchain's
    // own format decides what reaches the screen (Attachments.cpp counts it out).
    //
    // Tone mapping is what changes this -- it wants a float format, and the capture
    // above refuses one.
    constexpr VkFormat kRenderColorFormat = VK_FORMAT_R8G8B8A8_SRGB;

    // A value, because how big these are is a policy. The panel switches between two:
    // fixed, and following the window. Unreal keeps three behind
    // r.SceneRenderTargetResizeMethod and calls the trade memory against allocation
    // stalls.
    VkExtent2D renderExtent{kRenderWidth, kRenderHeight};

    // Contract: square, because lightProj below is a box with equal sides.
    constexpr VkExtent2D kShadowExtent{kShadowResolution, kShadowResolution};

    // The camera is not here. It keeps the desc it is made from (Passes.h), so its
    // lens and the target that shapes it cannot become two values that drift apart --
    // the loop builds it below the acquire, from whatever renderExtent is by then.
    //
    // Orthographic because the light is directional: parallel rays have no eye point
    // to project from, only a box, and the box decides how much world one texel covers.
    // Every argument is a constant, so this is built once.
    const glm::mat4 lightProj =
        glm::ortho(-kShadowRadius, kShadowRadius, -kShadowRadius, kShadowRadius,
                   0.1f, kShadowDistance * 2.0f);

    // The two values a caller cannot decide. Everything else about a target is ours.
    TargetCapabilities caps;
    if (!QueryTargetCapabilities(inst, selection.gpu, kRenderColorFormat,
                                 kDesiredSampleCount, &caps)) { return 1; }

    // What each pass draws into, written here beside everything else a pass is handed.
    // A TextureDesc says four things where AttachmentFormats says two, and the two it
    // adds are the ones that were missing: the extent a projection answers to, and
    // usage -- in which **SAMPLED marks an edge**. Exactly two of these four images
    // carry it, and those are the two another pass reads.
    SceneTargetDescs sceneTargets = MakeSceneTargets(renderExtent, kRenderColorFormat,
                                                     caps.depthFormat, caps.samples);
    const TextureDesc shadowTarget = MakeShadowTarget(kShadowExtent, caps.depthFormat);

    // Derived, not decided. Every field is one of the descs', so the images and the
    // pipelines compiled for them cannot come to disagree.
    const AttachmentFormats sceneFormats{sceneTargets.color.format,
                                         sceneTargets.depth.format,
                                         sceneTargets.color.samples};
    const AttachmentFormats shadowFormats{.depth = shadowTarget.format};

    // selection is absorbed here and not kept -- nothing below this line reads it.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }
    if (!CreateCommands(dev, &commands)) { return 1; }

    // Here, not with the passes below: the descriptor pool has to be told about this
    // one's set before it is created, and the font it points at is uploaded in here.
    if (!CreateGui(dev, commands, window, &renderer.guiPass)) { return 1; }

    // Passes -- a program per pair of shaders, a pipeline per variant of one
    // ------------------------------------------------------------------------
    //
    // Four programs and five pipelines: the scene has two, fill and line, and they
    // differ in polygonMode, which is compiled in. Anything that is a register instead
    // is dynamic state and costs no second pipeline.
    //
    // The depth-only pass, first because the scene pass reads what it draws.
    //
    // No colour format at all: the fragment stage declares no outputs, and the two
    // have to agree. One sample, because a visibility test cannot be averaged.
    if (!CreateShaderProgram(dev, "Shaders/shadow.vert.spv", "Shaders/shadow.frag.spv",
                             &renderer.shadowProgram)) { return 1; }

    // The same layout the scene pipeline gets: it describes the buffer, and
    // shadow.vert reads one location out of it. Which ones a pipeline consumes is the
    // vertex stage's answer, and CheckVertexInterface reads it from the .spv.
    //
    // One field, because one is all this pass decides.
    //
    // No colour: shadow.frag declares no output, and CreateGraphicsPipeline refuses
    // the pair where one side says colour and the other does not -- so writing
    // UNDEFINED here would be saying a second time what the .spv already settles.
    // One sample: the default, and averaging a visibility test would produce a depth
    // no surface was ever at.
    //
    // It goes into the pipeline and nowhere else. CreateShadowPass reads it back out
    // of there, so this value is written once and the images cannot be made from a
    // different one.

    GraphicsPipelineDesc shadowDesc;
    shadowDesc.vertexLayout = VertexInput();
    shadowDesc.formats = shadowFormats;
    if (!CreateGraphicsPipeline(dev, renderer.shadowProgram, shadowDesc,
                                &renderer.shadowPipeline)) { return 1; }

    // The program first: the shaders decide the set layouts and the push range, and a
    // pipeline only picks state on top of that. Two pipelines from one program share
    // every set already drawn from it.
    if (!CreateShaderProgram(dev, "Shaders/scene.vert.spv", "Shaders/scene.frag.spv",
                             &renderer.sceneProgram)) { return 1; }

    // Two lines, and they are the same two the shadow pipeline sets. What differs
    // between the passes is not compiled in any more: the viewport and the winding
    // that goes with it are set where the pass is recorded.
    GraphicsPipelineDesc opaqueDesc;
    opaqueDesc.vertexLayout = VertexInput();
    opaqueDesc.formats = sceneFormats;
    if (!CreateGraphicsPipeline(dev, renderer.sceneProgram, opaqueDesc,
                                &renderer.scenePipeline)) { return 1; }

    // The same desc with one field changed, which is the whole of what a second
    // variant is. LINE needs fillModeNonSolid, requested in Core.h and checked when
    // the GPU was picked.
    GraphicsPipelineDesc wireDesc = opaqueDesc;
    wireDesc.polygonMode = VK_POLYGON_MODE_LINE;
    if (!CreateGraphicsPipeline(dev, renderer.sceneProgram, wireDesc,
                                &renderer.sceneWirePipeline)) { return 1; }

    // The format was settled by SelectSurfaceFormat above and does not change, so this
    // pipeline is right from the start and nothing rebuilds it.
    //
    // No vertex input, no depth, 1 sample -- MSAA ended at the resolve.
    if (!CreateShaderProgram(dev, "Shaders/fullscreen.vert.spv",
                             "Shaders/fullscreen.frag.spv",
                             &renderer.presentProgram)) { return 1; }

    // What both swapchain passes draw into -- one value, because it is one image: the
    // gui pass draws on top of what the post pass leaves.
    //
    // Asked for rather than assembled. Every field of it is the presentation engine's
    // answer, which is the opposite of sceneFormats above, and the same type carries
    // both -- so the call is what says which.
    const AttachmentFormats swapchainFormats = SwapchainAttachmentFormats(window);

    GraphicsPipelineDesc presentDesc;
    presentDesc.formats = swapchainFormats;
    if (!CreateGraphicsPipeline(dev, renderer.presentProgram, presentDesc,
                                &renderer.presentPipeline)) { return 1; }

    // The panel. A different vertex type, a different set layout, and the only one
    // of the three that blends -- a window has to be see-through to be over anything.
    //
    // Same target as present, so the same format and 1 sample. y-down because ImGui
    // works in window pixels with the origin at the top left.
    if (!CreateShaderProgram(dev, "Shaders/gui.vert.spv", "Shaders/gui.frag.spv",
                             &renderer.guiProgram)) { return 1; }

    GraphicsPipelineDesc guiDesc;
    guiDesc.vertexLayout = GuiVertexInput();
    guiDesc.formats = swapchainFormats;
    guiDesc.blending = Blending::Translucent;
    if (!CreateGraphicsPipeline(dev, renderer.guiProgram, guiDesc,
                                &renderer.guiPipeline)) { return 1; }

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

    // glTF gives Sponza in centimetres and puts the scale on its one node. Applied
    // here rather than baked into the positions so the file stays the source of truth.
    constexpr float kSponzaScale = 0.008f;
    const glm::mat4 sceneModel = glm::scale(glm::mat4(1.0f), glm::vec3{kSponzaScale});
    for (DrawItem& item : items) { SetDrawModel(&item, sceneModel); }

    // The same layout the scene pipeline was built with, said once here and compared
    // in CreateScenePass. It used to be sizeof(Vertex) alone, which agreed with the
    // pipeline by habit rather than by anything.
    const MeshDesc meshDesc{VertexInput(),
                            static_cast<uint32_t>(vertices.size()),
                            static_cast<uint32_t>(indices.size())};
    if (!CreateMesh(dev, commands, meshDesc, vertices.data(), indices.data(),
                    &renderer.mesh)) {
        return 1;
    }

    // Textures -- the images the materials name
    // ------------------------------------------------------------------------
    //
    // Two per material, laid out in pairs. One pair per material the file named and
    // no extra: every primitive names a material, which the loader now insists on.
    //
    // A material missing one image gets a neutral texture rather than a null. The set
    // has bindings that all have to point somewhere; a null would be a validation
    // error at bind time, and a branch in the shader would be a third way to say the
    // same thing.
    //
    // URIs are relative to the .gltf, per the spec, and Sponza keeps its images
    // beside it.
    //
    // Three per material, laid out in runs: base colour, normal, metallic-roughness.
    // The count is here rather than spelled out at each index, because the day a
    // fourth arrives this is the line that changes.
    constexpr size_t kTexturesPerMaterial = 3;
    const uint32_t materialCount = static_cast<uint32_t>(materialSources.size());
    renderer.textures.resize(static_cast<size_t>(materialCount) * kTexturesPerMaterial);

    // One white texel, for a material that names no base colour. glTF says such a
    // material is its baseColorFactor alone, and white is the texture that multiplies
    // to exactly that -- a pattern here would be inventing detail the file does not
    // have. A checker lived here for that reason and drew one nowhere in this asset:
    // all 25 materials name a base colour.
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
        Texture& base = renderer.textures[first];
        Texture& normal = renderer.textures[first + 1];
        Texture& metalRough = renderer.textures[first + 2];

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
    // Here and not up with the pipelines: the pool cannot be sized until the materials
    // have been counted, which is what a material being data costs.
    //
    // Three claims, and the counts come from three different places -- frames in
    // flight for the two frame sets, materials for the material set. How many
    // descriptors that is per set is not asked here: CreateDescriptors reads it off
    // the layout, so the second binding a material grew did not reach this line.
    const SetRequest setRequests[] = {
        {&renderer.shadowProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer.sceneProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer.sceneProgram.setLayouts[kMaterialSet], materialCount},
        {&renderer.presentProgram.setLayouts[kFrameSet], kFramesInFlight},
        // One, and counted by neither of the other two reasons: there is one font.
        {&renderer.guiProgram.setLayouts[0], 1},
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
        // Same for the numbers: a white factor and no cutoff, which is what glTF means
        // by leaving both out.
        const MaterialParams params = i < materialSources.size() ? materialSources[i].params
                                                                 : MaterialParams{};
        // The cast is for the braced init: the enum is int, the flags field is
        // unsigned, and that counts as narrowing here.
        const size_t first = static_cast<size_t>(i) * kTexturesPerMaterial;
        sources[i] = {&renderer.textures[first],
                      &renderer.textures[first + 1],
                      &renderer.textures[first + 2],
                      params,
                      static_cast<VkCullModeFlags>(doubleSided ? VK_CULL_MODE_NONE
                                                               : VK_CULL_MODE_BACK_BIT)};
    }

    if (!CreateGuiSet(renderer.descriptors, renderer.guiProgram, renderer.guiPipeline,
                      &renderer.guiPass)) { return 1; }

    renderer.materials.resize(materialCount);
    if (!CreateMaterials(dev, renderer.descriptors,
                         renderer.sceneProgram.setLayouts[kMaterialSet], sources.data(),
                         materialCount, renderer.materials.data())) { return 1; }

    // Join the two halves the loader had to hand back separately. An index rather than
    // a pointer, so this survives renderer.materials moving in memory -- only its
    // length matters, and the recorder checks every index against it.
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].material = itemMaterial[i];
    }

    // The draw order, now that both things a bind depends on hang off one pointer.
    //
    // Only grouping matters, not which group comes first: a bind happens where two
    // neighbours differ, so any total order that puts equal materials together reaches
    // the same count. Comparing the indices is enough, and unlike the addresses they
    // replaced they are an order we chose -- two runs sort the same way.
    //
    // Cull first, even though it is a function of the material and so adds no
    // information. It adds an ordering: sorting on the material alone leaves the cull
    // groups interleaved, and grouping by the coarser value first costs nothing,
    // because a material never straddles two cull modes.
    //
    // Stable, so items that tie keep the order the file gave them. Nothing depends on
    // it yet -- blending is off, so no draw has to come after another.
    const std::vector<Material>& materials = renderer.materials;
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
                            renderer.materials.data(), materialCount};

    // Frames -- per-frame values, then the passes that read them, then the slots
    // ------------------------------------------------------------------------
    //
    // In dependency order: the scene pass's sets name the shadow maps, the post pass's
    // name what the scene pass made. A slot owns none of it and only knows its index.
    // No formats here. Each pass reads them off its pipeline, which is the thing that
    // baked them in -- passing them again would only make a second value to disagree.
    // Before the passes that read them: the shadow pass is created first, so a buffer
    // owned by either pass would have to exist before its owner.
    if (!CreateFrameCameras(dev, renderer.cameras)) { return 1; }
    if (!CreateFrameLights(dev, renderer.lights)) { return 1; }
    if (!CreateFrameShadows(dev, renderer.shadows)) { return 1; }

    if (!CreateShadowPass(dev, renderer.descriptors, shadowTarget,
                          renderer.mesh, renderer.shadowProgram,
                          renderer.shadowPipeline, renderer.shadows,
                          &renderer.shadowPass)) { return 1; }
    // What the scene pass reads of the shadow pass, and the only thing it reads.
    const Texture* shadowMaps[kFramesInFlight]{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        shadowMaps[i] = &renderer.shadowPass.frames[i].depth;
    }
    if (!CreateScenePass(dev, renderer.descriptors, sceneTargets,
                         renderer.mesh, renderer.sceneProgram, renderer.scenePipeline,
                         renderer.sceneWirePipeline,
                         shadowMaps, renderer.cameras, renderer.lights,
                         renderer.shadows, renderer.guiPass,
                         &renderer.scenePass)) { return 1; }
    // Both readers of the scene's colour take it from here -- the post pass and the
    // capture at the bottom of the loop. colorResolve and not color: a multisample
    // image cannot be sampled.
    const Texture* sceneColor[kFramesInFlight]{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        sceneColor[i] = &renderer.scenePass.frames[i].colorResolve;
    }
    if (!CreatePostProcessPass(renderer.descriptors, sceneColor,
                               renderer.presentProgram, renderer.presentPipeline,
                               &renderer.postPass)) {
        return 1;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateFrameSlot(dev, commands, i, &renderer.slots[i])) { return 1; }
    }


    LOG("close the window to exit. The panel switches features off.\n");

    // Frame state -- what the loop carries across frames
    // ------------------------------------------------------------------------
    //
    // Not a struct: only lookAt reads the camera values, so grouping would enforce
    // nothing.
    uint32_t slotIndex = 0;       // which slot this frame borrows
    double lastTime = glfwGetTime();

    // What the item order costs in state changes. Outside the loop because the panel
    // is built before RecordFrame fills it, so what it shows is the last frame's --
    // honest only because the list does not change between frames.
    DrawStats drawStats;
    bool loggedDrawStats = false;

    // Set it to a path and the first frame is written there and the program exits.
    // Implies fixed time -- a capture of a moving light compares against nothing.
    const char* const capturePath = std::getenv("LAMBDA_CAPTURE");

    // The light is the only thing here that reads absolute time, so two runs never
    // draw the same picture unless this stops it. Environment variables rather than
    // Config constants: a capture and a person running this want opposite values.
    //
    // Read once, and dt still comes from the real clock or the camera stops answering.
    const bool fixedTime =
        std::getenv("LAMBDA_FIXED_TIME") != nullptr || capturePath != nullptr;
    constexpr float kFixedTime = 1.0f;   // any constant. 1.0 puts the light off-axis

    // Inside the atrium, looking along it. The old value put the camera at the origin
    // facing -z, which is a wall from here -- it was chosen when the scene was five
    // spheres around the origin.
    glm::vec3 eye{-7.0f, 5.5f, 0.0f};
    float yaw = 0.0f;             // 0 looks down +x, per the forward expression below
    float pitch = -12.0f;         // the atrium floor, from the height of its gallery

    while (glfwWindowShouldClose(window.handle) == 0) {
        glfwPollEvents();

        // Sleep until an event arrives while minimized.
        //
        // Reset the clock after waking: the sleep is not a frame, and counting it
        // would make the next dt jump and teleport the camera.
        // The window's size, asked of the surface. Also the minimize check: 0x0 is
        // what a minimized surface reports, and skipping here is what keeps
        // EnsureSwapchain from retrying a device wait and a creation every iteration
        // with no present to pace it.
        if (!QuerySurfaceExtent(inst, dev.gpu, &window)) {
            glfwWaitEvents();
            lastTime = glfwGetTime();
            continue;
        }

        // Resize -- if the policy says the targets should be another size
        //
        // Above the acquire, because the size no longer arrives with an image: the
        // query above is where a window's size comes from now. That is what lets the
        // camera be built with the rest of the frame's state rather than after it.
        //
        // vkDeviceWaitIdle and not a fence: a fence covers one slot, and these images
        // belong to every slot.
        const VkExtent2D wanted = GuiRenderFollowsWindow(renderer.guiPass)
                                ? window.surfaceExtent
                                : VkExtent2D{kRenderWidth, kRenderHeight};

        if (wanted.width != renderExtent.width || wanted.height != renderExtent.height) {
            dev.table.vkDeviceWaitIdle(dev.handle);

            // Described again at the new size, by the same call that described them
            // the first time. Only the extent differs.
            renderExtent = wanted;
            sceneTargets = MakeSceneTargets(renderExtent, kRenderColorFormat,
                                            caps.depthFormat, caps.samples);

            if (!ResizeScenePass(dev, sceneTargets, &renderer.scenePass)) { break; }

            // The only sets that name what was just destroyed.
            RefreshPostProcessPass(renderer.descriptors, &renderer.postPass);
            LOG("[render] targets now %ux%u\n", renderExtent.width, renderExtent.height);
        }

        // What to draw
        // --------------------------------------------------------------------
        //
        // No GPU call in any of it, so it could run while minimized. What comes out is
        // state -- a camera, a light, a list -- and the next section sends it.

        // Clock
        //
        // One clock reading, two values: t is absolute (object spin), dt is the gap
        // (camera movement). Reading twice would let them drift apart.
        const double now = glfwGetTime();
        const float t = fixedTime ? kFixedTime : static_cast<float>(now);
        const float dt = static_cast<float>(now - lastTime);
        lastTime = now;

        // Camera -- input to a view
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


        // Light
        //
        // One directional light, circling so the brightness visibly changes -- the
        // objects turn about z, which leaves their normals fixed.
        // y is 3.0, not the 0.5 it was before there was a shadow, and it was measured
        // rather than chosen: at 0.5 and at 1.4 the arcades block the sun before it
        // reaches the open middle and the whole scene reads as one flat dark mass.
        // From here the light comes down the courtyard and the columns cast across it.
        //
        // xz still turn with t, so what moves is the direction the shadows fall.
        const glm::vec3 lightDir = glm::normalize(
            glm::vec3{std::cos(t) * 0.7f, 3.0f, std::sin(t) * 0.7f});

        // Where the light looks from. Its lens is up with the render targets, for the
        // reason the camera's is -- a projection answers to the image it lands on, and
        // this one lands on a square map that never changes size.
        //
        // The centre is fixed rather than fitted to the camera. Fitting is what a real
        // one does (and what cascades are), and it needs the frustum's corners in
        // light space; a constant box is honest about covering this scene and nothing
        // larger, and scene.frag returns "lit" for anything outside it.
        //
        // lightDir points from the surface toward the light, so the eye is the centre
        // plus that. It never lines up with world up -- y is fixed at 0.5 while xz go
        // round -- which is what keeps lookAt's cross product from collapsing.
        constexpr glm::vec3 kSceneCenter{0.0f, 3.0f, 0.0f};
        const glm::mat4 lightView =
            glm::lookAt(kSceneCenter + lightDir * kShadowDistance, kSceneCenter, kWorldUp);
        const glm::mat4 lightViewProj = lightProj * lightView;

        // Fill this frame's share of the pass
        //
        // Assignment only, so it belongs up here: what reaches the GPU, and when, is
        // RecordFrame's. Through slot.index and not slotIndex: recording picks the
        // pass's frame the same way.
        FrameSlot& slot = renderer.slots[slotIndex];

        // center is eye + forward: an absolute target would pin the gaze and rotation
        // would stop working. viewPos comes out of the desc rather than being copied
        // beside it -- one camera, one place its position is written down.
        const Camera camera = MakeCamera(CameraDesc{renderExtent,
                                                    kFovDegrees, kNearPlane, kFarPlane,
                                                    eye, forward, kWorldUp});
        renderer.cameras[slot.index].value =
            {camera.proj * camera.view, glm::vec4{camera.desc.eye, 0.0f}};

        // What reaches a surface, and where its shadow map was drawn from.
        renderer.lights[slot.index].value =
            {glm::vec4{lightDir, 0.0f}, glm::vec4{1.0f, 0.95f, 0.9f, 0.15f}};
        renderer.shadows[slot.index].value = {lightViewProj};

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

        // Past the acquire, a failure breaks rather than continues: the image is
        // taken, imageAvailable is signalled and the fence is about to be reset, and
        // skipping the submit leaves both with nobody to wait on them.

        // The panel, after the acquire because it reports the image this frame got.
        // Nothing here touches the GPU -- it only fills a draw list that
        // RecordGuiPass reads. Below the Skip return on purpose: a frame that is not
        // recorded would leave that list stale.
        GuiFrameInfo guiInfo;
        guiInfo.frameSeconds = dt;
        guiInfo.itemCount = static_cast<uint32_t>(items.size());
        guiInfo.materialCount = materialCount;
        guiInfo.recordedDraws = drawStats.draws;
        guiInfo.materialBinds = drawStats.materialBinds;
        guiInfo.cullChanges = drawStats.cullChanges;
        guiInfo.descriptors = &renderer.descriptors;
        guiInfo.sceneProgram = &renderer.sceneProgram;
        guiInfo.presentProgram = &renderer.presentProgram;
        guiInfo.guiProgram = &renderer.guiProgram;
        guiInfo.scenePipeline = &renderer.scenePipeline;
        guiInfo.presentPipeline = &renderer.presentPipeline;
        guiInfo.cameraBytes = static_cast<uint32_t>(sizeof(CameraUniform));
        guiInfo.lightBytes = static_cast<uint32_t>(sizeof(LightUniform)
                                                   + sizeof(ShadowUniform));
        guiInfo.pushBytes = static_cast<uint32_t>(sizeof(PushConstants));
        guiInfo.vertexStride = renderer.mesh.desc.vertexLayout.stride;
        guiInfo.vertexAttributes = renderer.mesh.desc.vertexLayout.attributeCount;
        guiInfo.framesInFlight = kFramesInFlight;
        guiInfo.mesh = &renderer.mesh;
        guiInfo.guiPipeline = &renderer.guiPipeline;
        guiInfo.slotIndex = slot.index;
        guiInfo.sceneColor = &renderer.scenePass.frames[slot.index].color;
        guiInfo.sceneResolve = &renderer.scenePass.frames[slot.index].colorResolve;
        guiInfo.sceneDepth = &renderer.scenePass.frames[slot.index].depth;
        guiInfo.frameTarget = target.texture;
        BuildGui(&renderer.guiPass, guiInfo);


        // Only the texture: recording has no use for the rest of the target.
        //
        // Reset rather than declared here: the counters add up, and the panel above
        // read last frame's values before this line overwrites them.
        drawStats = DrawStats{};
        if (!RecordFrame(slot, renderer.cameras, renderer.lights, renderer.shadows,
                         renderer.shadowPass, renderer.scenePass,
                         renderer.postPass,
                         renderer.guiPass, *target.texture, drawList, &drawStats)) {
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
            const Texture& shot = *sceneColor[slot.index];

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
