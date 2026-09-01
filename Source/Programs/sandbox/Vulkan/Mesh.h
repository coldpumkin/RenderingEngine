#pragma once

#include "Vulkan/Buffer.h"

// Mesh - the two buffers a draw always binds together
// ============================================================================
//
// What has to agree, and who says so when it does not:
//
//   index value    -> a slot in this vertex buffer   nobody  <- this type removes the pair
//   indexType      -> element type of the indices    nobody  <- CreateMesh takes uint16 only
//   vertex layout  -> the pipeline's vertexInput     nobody  <- Contract, see Pipeline.h
//
// No index range here: a span is "which object", and Vulkan/ does not know objects.
struct Mesh {
    Buffer vertices;
    Buffer indices;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;   // CreateMesh derives it
};

// Input:  vertices as raw bytes, indices as typed elements
// Output: out filled; on failure ~Buffer frees whatever was already made
// Effect: staging upload for both. Blocks until done -- init path.
//
// Asymmetric on purpose: vertex layout belongs to the pipeline, index type to nobody.
bool CreateMesh(const VulkanDevice& dev,
                const Commands& commands,
                const void* vertices, VkDeviceSize vertexBytes,
                const uint16_t* indices, uint32_t indexCount,
                Mesh* out) noexcept;
