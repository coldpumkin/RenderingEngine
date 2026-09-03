#pragma once

#include "Vulkan/Buffer.h"
#include "Vulkan/VertexLayout.h"   // what these bytes were written as

// Mesh - the two buffers a draw always binds together
// ============================================================================
//
// Three things must agree; the desc settles the first two:
//
//   index value    -> a slot in this vertex buffer   the pair cannot be split
//   indexType      -> element type of the indices
//   vertex layout  -> the pipeline's vertexLayout     compared, not assumed
//
// The layout is here rather than a bare stride because these bytes were written as
// something, and the pipeline that reads them was built for something. Both sides now
// say what, so CreateScenePass can compare them instead of hoping.
//
// No index range here: a span is "which object", which Vulkan/ does not know.
struct MeshDesc {
    VertexLayout vertexLayout;   // VertexInput() at the call site
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
