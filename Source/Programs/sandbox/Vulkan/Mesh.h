#pragma once

#include "Vulkan/Buffer.h"

// Mesh - the two buffers a draw always binds together
// ============================================================================
//
// Both were plain Buffer arguments passed side by side. They are one type because
// an index is meaningless without the vertex buffer it counts into: nothing stops
// a caller from pairing indices with the wrong vertices, and neither the compiler
// nor the validation layer says a word. Holding them in one type removes the pair.
//
// What has to agree, and who tells us when it does not:
//
//   index value    -> a slot in this vertex buffer   nobody   <- this type removes it
//   indexType      -> element type of the indices    nobody   <- CreateMesh takes uint16 only
//   vertex layout  -> the pipeline's vertexInput     nobody   <- Contract, see Pipeline.h
//
// The third one stays open. Layout lives in GLSL and in C++ and no compiler reads
// both, so a mesh cannot check it -- only the pipeline it is drawn with knows.
//
// Not here: what an index range means. A span of the index buffer is a scene
// question (which object is this?), not a GPU one, and it comes from whatever
// builds the scene. Vulkan/ does not know about objects.
struct Mesh {
    Buffer vertices;
    Buffer indices;

    // Derived from the index element type, not chosen. CreateMesh takes uint16
    // indices, so this has one value until a loader hands us uint32 ones.
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
};

// Uploads both buffers to device-local memory (staging inside, one at a time).
//
// Input:  vertices as raw bytes, indices as typed elements
// Output: out filled; on failure the buffers already made are freed by ~Buffer
// Effect: blocks until both copies finish. Init path, so waiting is fine.
//
// The two sides are asymmetric on purpose. Vertices come as bytes because the
// layout is the pipeline's business and this function must not have an opinion on
// it. Indices come typed because that type is exactly what nobody else checks.
bool CreateMesh(const VulkanDevice& dev,
                const Commands& commands,
                const void* vertices, VkDeviceSize vertexBytes,
                const uint16_t* indices, uint32_t indexCount,
                Mesh* out) noexcept;
