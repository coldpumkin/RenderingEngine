#include "Gltf.h"

#include <cgltf.h>

#include <cmath>       // sqrt, for the tangent basis
#include <cstring>     // strcmp on attribute names

#include <glm/glm.hpp>

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
bool LoadGltf(const char* path,
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

            // glTF requires min and max on the POSITION accessor, so this is read
            // rather than computed. An asset that omits them anyway leaves the item's
            // empty box, which the culler reads as always visible.
            if (pos->has_min && pos->has_max) {
                item.boundsMin = glm::vec3{pos->min[0], pos->min[1], pos->min[2]};
                item.boundsMax = glm::vec3{pos->max[0], pos->max[1], pos->max[2]};
            }
            items->push_back(item);
        }
    }

    LOG("[gltf] %s: %zu primitives, %zu vertices, %zu indices, %zu materials\n",
        path, items->size(), vertices->size(), indices->size(), materialSources->size());

    cgltf_free(data);
    return !items->empty();
}
