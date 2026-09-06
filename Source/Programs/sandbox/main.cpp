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


#include "Config.h"
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

// What the loader found for one material, and the key two of them are compared on.
//
// An empty path means the material named no image there, and the caller substitutes a
// neutral one -- white for the base colour, flat for the normal. White because glTF
// says a material without a baseColorTexture is its factor alone, and white multiplies
// to exactly that.
//
// **Every field that makes two materials different has to be in here.** Cull is, or
// two materials naming the same images and differing in double_sided would quietly
// become one. The factors are the next thing missing, and they collapse the same way.
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
// The image itself, read off the GPU -- not a screenshot of the window, which reads
// whatever is at those coordinates (the same binary measured 0.95% and 14.26% black on
// two runs). So two runs of one build are identical and a diff is only ever code.
//
// Contract: the subject is the presented image, at the window's size and with every
//           pass in it. Two captures compare only if taken the same way, so moving the
//           subject changes every recorded hash at once -- which is what happened when
//           it stopped being the scene's resolve.

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
// glTF makes TANGENT optional and says a runtime must generate it. Sponza has exactly
// one primitive without it, and that material names no normal map -- so nothing built
// here reaches the picture today. Generated anyway because **zero is not neutral**:
// the TBN's first column would be normalize(0), which is NaN.
//
// Across one triangle the surface is a plane, so uv is affine in position and the
// tangent is the direction u grows in:
//
//   T = (dp1 * dv2 - dp2 * dv1) / (du1 * dv2 - du2 * dv1)
//
// Accumulated per vertex and normalized after, which averages the seams. Not
// MikkTSpace -- that splits vertices to keep mirrored uvs exact.
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
// A DrawItem carries an index and not the Material: a set does not exist until the
// pool does, and the pool cannot be sized until this has counted them.
//
// Indices stay uint16 though the buffer holds far more than 65535 vertices: each
// primitive numbers from its own first vertex, which the DrawItem carries as
// vertexOffset. What has to fit in 16 bits is one primitive, and Sponza's largest is
// 23,038.
//
// Node transforms are not walked -- Sponza is one node with a scale, and the caller
// multiplies it in. cgltf_node_transform_world is where nesting would go.
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

            // The third link in a chain that had two: the .spv against the layout,
            // the layout against the mesh, and **nothing against the asset**. A file
            // missing an attribute got the zeroes resize left behind, and a zero
            // normal is not dark -- normalize() of it is NaN.
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

            // Both are the material's, and they land in different places: the cutoff
            // is a number the shader compares, cull is rasterizer state. They move
            // together in this asset -- all 3 MASK materials are double sided -- and
            // glTF does not say they must, so they are read apart.
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

            // Keyed on the whole pair, not on cgltf_material and not on base colour
            // alone: same two images is one entry, same base colour with a different
            // normal map is two.
            //
            // A primitive naming no material is legal glTF -- draw it with the default
            // -- and that default is a thing we would build and never use. Refused
            // instead, the way a missing NORMAL is.
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

                // Every material the file names gets an entry, images or not: one
                // with a coloured factor and no textures is ordinary glTF, and its
                // colour, cutoff and double_sided were read three lines above.
                //
                // The stand-in is for a primitive that names no material at all --
                // the one case with nothing to carry.
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

    // Display -- a window, and a GPU that can drive it
    // ========================================================================

    // glfwInit is first only because windowSystem is declared first and so dies last.
    if (!InitWindowSystem(&windowSystem)) { return 1; }
    if (!CreateInstance(&inst)) { return 1; }
    if (!OpenWindow(inst, kWindowWidth, kWindowHeight, "Lambda Engine", &window)) {
        return 1;
    }

    // Which GPU, and which of its queue families. Nothing about what we draw.
    const PhysicalDeviceSelection selection = PickPhysicalDevice(inst, window.surface);
    if (selection.gpu == VK_NULL_HANDLE) { return 1; }

    // Must be sRGB: that encode happens nowhere else in the chain. The offscreen
    // colour still does not follow from it -- Attachments.cpp counts the cases.
    if (!SelectSurfaceFormat(inst, selection.gpu, &window)) { return 1; }

    // Beside the format because it is the same kind of value: settled once, read on
    // every swapchain recreation. **Written here and not read down there** -- how many
    // images to rotate is our policy, and Vulkan/ includes nothing above itself.
    window.desiredImages = kDesiredSwapchainImages;

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

    // Device -- and past it, everything that needs one
    // ========================================================================

    // selection is absorbed here and not kept -- nothing below reads it.
    if (!CreateDevice(inst, selection, &dev)) { return 1; }
    if (!CreateCommands(dev, &commands)) { return 1; }

    // Here and not with the passes: the descriptor pool has to be told about this
    // one's set before it is created, and the font it points at is uploaded here.
    if (!CreateGui(dev, commands, window, &renderer.guiPass)) { return 1; }

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
    if (!CreatePipelines(dev, pipelineSources, &renderer.pipelines)) { return 1; }

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
    // Here and not with the pipelines: the pool cannot be sized until the materials
    // are counted.
    //
    // Three claims from three different counts -- frames in flight for the two frame
    // sets, materials for the material set. How many descriptors each set holds is
    // read off the layout by CreateDescriptors, not written here.
    const SetRequest setRequests[] = {
        {&renderer.pipelines.shadowProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer.pipelines.sceneProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer.pipelines.sceneProgram.setLayouts[kMaterialSet], materialCount},
        // The deferred half. geometry's set 0 is two bindings with holes between them;
        // lighting's is the same five the scene's is, and its set 1 is the g-buffer
        // rather than a material -- which is why it is claimed here and the material
        // sets above are not counted twice. **geometry draws no material sets of its
        // own**: it speaks MaterialSet(), so the ones the scene pass uses fit it.
        {&renderer.pipelines.geometryProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer.pipelines.lightingProgram.setLayouts[kFrameSet], kFramesInFlight},
        {&renderer.pipelines.lightingProgram.setLayouts[kMaterialSet], kFramesInFlight},
        {&renderer.pipelines.postProgram.setLayouts[kFrameSet], kFramesInFlight},
        // One, and counted by neither of the other two reasons: there is one font.
        {&renderer.pipelines.guiProgram.setLayouts[0], 1},
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

    if (!CreateGuiSet(renderer.descriptors, renderer.pipelines.gui, swapchainTarget,
                      &renderer.guiPass)) { return 1; }

    renderer.materials.resize(materialCount);
    if (!CreateMaterials(dev, renderer.descriptors,
                         renderer.pipelines.sceneProgram.setLayouts[kMaterialSet], sources.data(),
                         materialCount, renderer.materials.data())) { return 1; }

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
    //
    // The buffers come before both passes that read them, which is also why neither
    // owns them -- the shadow pass is created first and would have to outlive itself.
    if (!CreateFrameCameras(dev, renderer.cameras)) { return 1; }
    if (!CreateFrameLights(dev, renderer.lights)) { return 1; }
    if (!CreateFrameShadows(dev, renderer.shadows)) { return 1; }
    if (!CreateFrameViewOptions(dev, renderer.viewOptions)) { return 1; }

    // The shadow map, made here from the desc written at the top and handed to both
    // passes that touch it -- the one that draws it and the one that samples it. The
    // edge between them is this array, not a walk into whichever pass owned the image.
    const Texture* shadowMaps[kFramesInFlight]{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateTexture(dev, shadowTarget, &renderer.shadowMaps[i])) { return 1; }
        shadowMaps[i] = &renderer.shadowMaps[i];
    }

    if (!CreateShadowPass(renderer.descriptors, shadowMaps,
                          renderer.mesh,
                          renderer.pipelines.shadow, renderer.shadows,
                          &renderer.shadowPass)) { return 1; }
    // The scene's three, made here and named here. The pass draws into them, the post
    // pass samples the resolve, and the capture reads the same image -- three readers
    // and no pass in the middle of any of them.
    const SceneTargets* sceneTargets[kFramesInFlight]{};
    const Texture* sceneColor[kFramesInFlight]{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateSceneTargets(dev, sceneTargetDescs, &renderer.sceneTargets[i])) {
            return 1;
        }
        sceneTargets[i] = &renderer.sceneTargets[i];

        // resolve and not color: a multisample image cannot be sampled.
        sceneColor[i] = &renderer.sceneTargets[i].resolve;
    }

    if (!CreateScenePass(renderer.descriptors, sceneTargets,
                         renderer.mesh, renderer.pipelines.scene,
                         renderer.pipelines.sceneWire,
                         shadowMaps, renderer.cameras, renderer.lights,
                         renderer.shadows, renderer.viewOptions,
                         &renderer.scenePass)) { return 1; }
    if (!CreatePostProcessPass(renderer.descriptors, sceneColor, swapchainTarget,
                               renderer.pipelines.post,
                               &renderer.postPass)) {
        return 1;
    }

    // The deferred middle. Both chains exist from here on and neither is rebuilt when
    // the panel switches -- what the switch changes is which of them RecordFrame
    // names.
    const GBufferTargets* gbuffers[kFramesInFlight]{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateGBufferTargets(dev, gbufferDescs, &renderer.gbuffers[i])) {
            return 1;
        }
        gbuffers[i] = &renderer.gbuffers[i];
    }

    if (!CreateGeometryPass(renderer.descriptors, gbuffers,
                            renderer.mesh, renderer.pipelines.geometry,
                            renderer.pipelines.geometryWire,
                            renderer.cameras, renderer.viewOptions,
                            &renderer.geometryPass)) { return 1; }

    // sceneColor is the scene pass's resolve, and this pass draws into it rather than
    // reading it. Only one of the two ever writes it in a frame.
    if (!CreateLightingPass(renderer.descriptors, gbuffers, sceneColor,
                            renderer.pipelines.lighting,
                            shadowMaps, renderer.cameras, renderer.lights,
                            renderer.shadows, renderer.viewOptions,
                            &renderer.lightingPass)) { return 1; }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CreateFrameSlot(dev, commands, i, &renderer.slots[i])) { return 1; }
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

    // Built once, because none of its inputs move -- see ShadowProjectionFor, which
    // takes no state at all.
    const glm::mat4 lightProj = ShadowProjectionFor(shadowTarget);

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

    // Which frame a capture is of. 1 and not 0 because the panel is not in frame 0 --
    // see the capture itself, below the submit.
    constexpr uint32_t kCaptureFrame = 1;
    uint32_t framesDrawn = 0;
    constexpr float kFixedTime = 1.0f;   // any constant. 1.0 puts the light off-axis

    // Inside the atrium, looking along it. The old value put the camera at the origin
    // facing -z, which is a wall from here -- it was chosen when the scene was five
    // spheres around the origin.
    glm::vec3 eye{-7.0f, 5.5f, 0.0f};
    float yaw = 0.0f;             // 0 looks down +x, per the forward expression below
    float pitch = -12.0f;         // the atrium floor, from the height of its gallery

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
                                            &renderer.sceneTargets[i])
                      && ResizeGBufferTargets(dev, gbufferDescs,
                                              &renderer.gbuffers[i]);
            }
            if (!remade) { break; }

            // The sets that name what was just destroyed. Two now: the post pass
            // reads the resolve, and the lighting pass's second set names all four
            // g-buffer views. The lighting pass's first set survives -- it names
            // buffers and the shadow map, and a resize touches neither.
            RefreshPostProcessPass(renderer.descriptors, &renderer.postPass);
            RefreshLightingPass(renderer.descriptors, &renderer.lightingPass);
            LOG("[render] targets now %ux%u\n", wanted.width, wanted.height);
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
        //
        // Both fixed together, and dt matters as much as t: the panel prints a frame
        // time, so a real one puts the machine's speed into the picture. Two runs of
        // one build differed by that alone until this line existed. 1/60 rather than 0
        // -- a zero gap is a frame nothing could have moved in.
        const double now = glfwGetTime();
        const float t = fixedTime ? kFixedTime : static_cast<float>(now);
        const float dt = fixedTime ? 1.0f / 60.0f
                                   : static_cast<float>(now - lastTime);
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

        // A limit now, and not a collapse. Nothing below crosses two vectors, so +-90
        // is a representable orientation; this stays because a camera that tips past
        // vertical is not what these four keys are for.
        pitch = glm::clamp(pitch, -89.0f, 89.0f);

        // What the renderer is handed: the orientation itself, rather than a direction
        // read off it. Built from the two angles each frame and not accumulated -- a
        // running product of small turns drifts and needs renormalizing, and these two
        // angles are already the whole of what the keys change.
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
        // One directional light, circling in xz so the shadows sweep. y is fixed at
        // 3.0: measured, not chosen -- at 0.5 and 1.4 the arcades cut the sun off
        // before the courtyard and the scene reads as one flat dark mass. It no longer
        // holds a second job: the degenerate up that used to depend on it is chosen
        // inside ShadowView now.
        const LightState light{
            glm::normalize(glm::vec3{std::cos(t) * 0.7f, 3.0f, std::sin(t) * 0.7f}),
            glm::vec3{1.0f, 0.95f, 0.9f},
            0.15f};
        // Fill this frame's share of the pass
        //
        // Assignment only, so it belongs up here: what reaches the GPU, and when, is
        // RecordFrame's. Through slot.index and not slotIndex: recording picks the
        // pass's frame the same way.
        FrameSlot& slot = renderer.slots[slotIndex];

        // State, and the image it is drawn into. Nothing here is a matrix: what the
        // camera is belongs to this loop, what it looks like to the GPU is made from
        // it, and the two arguments are that line.
        //
        // viewPos comes back out of the state rather than being copied beside it --
        // one camera, one place its position is written down.
        const CameraState camera{{eye, orientation}, kFovDegrees};

        // Named and not positional, in all three. Every one of these blocks has two
        // adjacent fields of the same type -- two mat4 here, two mat4 in the shadow,
        // two vec4 in the light -- so writing them in the wrong order compiles, draws
        // a wrong picture, and passes every check we have: reflection compares the
        // layout, not which matrix went in which slot. C++20 requires designators to
        // follow declaration order, so a transposition is a compile error instead.
        //
        // The camera's projection is rebuilt here and the light's is not, and the
        // difference is in what can move: this one answers to a target that resizes
        // and a field of view the app could change, and ShadowProjectionFor takes
        // nothing that changes at all. Held when nothing can move it, derived when
        // something can.
        renderer.cameras[slot.index].value =
            {.view = ViewFromPose(camera.pose),
             .proj = ProjectionFor(camera.fovDegrees, sceneTargetDescs.color),
             .viewPos = glm::vec4{camera.pose.position, 1.0f}};

        // What reaches a surface, and where its shadow map was drawn from. The second
        // is made here rather than held: it turns with the light every frame, while
        // lightProj above does not move at all.
        renderer.lights[slot.index].value =
            {.direction = glm::vec4{light.direction, 0.0f},
             .color = glm::vec4{light.color, light.ambient}};
        renderer.shadows[slot.index].value =
            {.lightView = ShadowView(light.direction, kSceneCenter),
             .lightProj = lightProj};

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
        guiInfo.sceneProgram = &renderer.pipelines.sceneProgram;
        guiInfo.postProgram = &renderer.pipelines.postProgram;
        guiInfo.guiProgram = &renderer.pipelines.guiProgram;
        guiInfo.scenePipeline = &renderer.pipelines.scene;
        guiInfo.postPipeline = &renderer.pipelines.post;
        guiInfo.cameraBytes = static_cast<uint32_t>(sizeof(CameraUniform));
        guiInfo.lightBytes = static_cast<uint32_t>(sizeof(LightUniform)
                                                   + sizeof(ShadowUniform));
        guiInfo.pushBytes = static_cast<uint32_t>(sizeof(PushConstants));
        guiInfo.vertexStride = renderer.mesh.desc.vertexLayout.stride;
        guiInfo.vertexAttributes = renderer.mesh.desc.vertexLayout.attributeCount;
        guiInfo.framesInFlight = kFramesInFlight;
        guiInfo.mesh = &renderer.mesh;
        guiInfo.guiPipeline = &renderer.pipelines.gui;
        guiInfo.slotIndex = slot.index;
        guiInfo.sceneColor = &renderer.sceneTargets[slot.index].color;
        guiInfo.sceneResolve = &renderer.sceneTargets[slot.index].resolve;
        guiInfo.sceneDepth = &renderer.sceneTargets[slot.index].depth;
        guiInfo.frameTarget = target.texture;
        BuildGui(&renderer.guiPass, guiInfo);


        // The values written above, into the buffers the sets already name. Here and
        // not inside RecordFrame: it is a memcpy per value, not a command.
        UploadFrameValues(slot, renderer.cameras, renderer.lights, renderer.shadows,
                          renderer.viewOptions, renderer.guiPass);

        // Only the texture: recording has no use for the rest of the target.
        //
        // Reset rather than declared here: the counters add up, and the panel above
        // read last frame's values before this line overwrites them.
        drawStats = DrawStats{};
        if (!RecordFrame(slot, renderer.shadowPass, renderer.scenePass,
                         renderer.geometryPass, renderer.lightingPass,
                         renderer.postPass, renderer.guiPass,
                         *target.texture, drawList, &drawStats)) {
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
