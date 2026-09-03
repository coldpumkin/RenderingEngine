#pragma once

// VertexLayout - what a vertex buffer hands the vertex stage
// ============================================================================
//
// Its own header for the reason AttachmentFormats has one: two unrelated things hold
// it. A pipeline is built for a layout and a mesh was written with one, and neither of
// those should have to include the other to say so.
//
// The same kind of thing AttachmentFormats is, at the other end of the shader: a
// resource-side description of a boundary, checked against what the .spv declares.
// Not merged with it -- what each adds beyond "location -> format" is its resource
// kind. A buffer needs where in it; an image needs how many samples.

#include "Vulkan/Shader.h"   // kMaxVertexAttributes

// One attribute: which shader location it feeds, in what format, at what byte offset
// inside one vertex.
struct VertexAttribute {
    uint32_t location = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t offset = 0;
};

// stride 0 means no vertex buffer at all -- a shader that builds its own points. In
// band the way AttachmentFormats says "no depth" with UNDEFINED: a flag beside it
// would make "no buffer, but here are four attributes" expressible.
struct VertexLayout {
    uint32_t stride = 0;
    uint32_t attributeCount = 0;
    VertexAttribute attributes[kMaxVertexAttributes]{};
};

// Effect: true when both describe the same bytes -- same stride, same attributes in
//         the same order.
//
// Not a subset test. A buffer carrying extra attributes the pipeline ignores would
// still be read at the same stride, but nothing here produces one, and accepting it
// would hide the mismatch this is for.
//
// inline rather than a .cpp: it is four comparisons, and a new translation unit would
// mean editing CMakeLists, which reconfigures in the wrong codepage (CLAUDE.md).
inline bool SameVertexLayout(const VertexLayout& a, const VertexLayout& b) noexcept {
    if (a.stride != b.stride || a.attributeCount != b.attributeCount) { return false; }
    for (uint32_t i = 0; i < a.attributeCount; ++i) {
        if (a.attributes[i].location != b.attributes[i].location
                || a.attributes[i].format != b.attributes[i].format
                || a.attributes[i].offset != b.attributes[i].offset) {
            return false;
        }
    }
    return true;
}
