#pragma once

// Loading a glTF file into the arrays a frame is built from
// ============================================================================
//
// This is the first of the four steps in Renderer.h's sentence -- an asset becoming
// something the GPU can be given -- and it stops one step short of the GPU. What comes
// out is CPU memory: vertices, indices, one DrawItem per primitive, and what each
// material named. Uploading them is the caller's, which is why nothing here takes a
// device.
//
// Everything is flattened into one vertex buffer and one index buffer. A primitive
// becomes a span rather than a buffer of its own, which is what lets 103 draws bind
// nothing between them.

#include "Passes.h"    // DrawItem, MaterialParams
#include "Vertex.h"

#include <cstdint>
#include <string>
#include <vector>

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

// Input:  path to the .gltf. Texture URIs come back relative to it, as the spec says
// Output: false on a missing or malformed file, and on an asset this renderer cannot
//         draw -- a primitive without NORMAL or TEXCOORD_0, or without a material.
//         The message says which
//
// itemMaterial is one index per item into materialSources, kept apart because the
// materials become descriptor sets only after the textures are uploaded, and the item
// cannot name a set that does not exist yet.
bool LoadGltf(const char* path,
              std::vector<Vertex>* vertices,
              std::vector<uint16_t>* indices,
              std::vector<DrawItem>* items,
              std::vector<uint32_t>* itemMaterial,
              std::vector<MaterialSource>* materialSources) noexcept;
