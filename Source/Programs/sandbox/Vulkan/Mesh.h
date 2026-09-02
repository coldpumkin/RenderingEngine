#pragma once

#include "Vulkan/Buffer.h"

// Mesh - the two buffers a draw always binds together
// ============================================================================
//
// Three things must agree; the desc settles the first two:
//
//   index value    -> a slot in this vertex buffer   the pair cannot be split
//   indexType      -> element type of the indices
//   vertex layout  -> the pipeline's vertexInput     open: only the stride matches up
//
// stride is what a mesh can say about its vertices: how many bytes one takes. Which
// bytes are position and which are uv is the pipeline's, because that is what the
// shader's locations answer to.
//
// No index range here: a span is "which object", which Vulkan/ does not know.
struct MeshDesc {
    uint32_t vertexStride = 0;   // sizeof(Vertex) at the call site
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
};

struct Mesh {
    MeshDesc desc;
    Buffer vertices;
    Buffer indices;
};

// Input:  desc, and the two arrays as raw bytes
// Effect: staging upload for both. Blocks until done -- init path.
//
// Sizes come from the desc, so no byte count is passed twice.
bool CreateMesh(const VulkanDevice& dev,
                const Commands& commands,
                const MeshDesc& desc,
                const void* vertices, const void* indices,
                Mesh* out) noexcept;
