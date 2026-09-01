#include "Vulkan/Mesh.h"

bool CreateMesh(const VulkanDevice& dev,
                const Commands& commands,
                const void* vertices, VkDeviceSize vertexBytes,
                const uint16_t* indices, uint32_t indexCount,
                Mesh* out) noexcept {
    // usage is not an argument. The caller has nothing to choose here: a mesh's two
    // buffers are a vertex buffer and an index buffer by definition.
    if (!CreateDeviceLocalBuffer(dev, commands, vertices, vertexBytes,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &out->vertices)) {
        return false;
    }

    // Size is derived from the element type rather than taken as bytes, which is the
    // same reason indexType is not an argument either.
    const VkDeviceSize indexBytes = VkDeviceSize{indexCount} * sizeof(uint16_t);
    if (!CreateDeviceLocalBuffer(dev, commands, indices, indexBytes,
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &out->indices)) {
        return false;
    }

    return true;
}
