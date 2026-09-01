#pragma once

#include "Vulkan/Buffer.h"

// Mesh - the two buffers a draw always binds together
// ============================================================================
//
// Three things must agree; this type settles the first two:
//
//   index value    -> a slot in this vertex buffer   the pair cannot be split
//   indexType      -> element type of the indices    CreateMesh takes uint16 only
//   vertex layout  -> the pipeline's vertexInput     open: nobody reads both sides
//
// No index range here: a span is "which object", which Vulkan/ does not know.
struct Mesh {
    Buffer vertices;
    Buffer indices;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
};

// Input:  vertices as raw bytes, indices as typed elements
// Effect: staging upload for both. Blocks until done -- init path.
//
// The asymmetry is Vulkan's: vkCmdBindVertexBuffers takes no type (the pipeline
// bakes the layout), vkCmdBindIndexBuffer takes one. One mesh, several shaders.
bool CreateMesh(const VulkanDevice& dev,
                const Commands& commands,
                const void* vertices, VkDeviceSize vertexBytes,
                const uint16_t* indices, uint32_t indexCount,
                Mesh* out) noexcept;
