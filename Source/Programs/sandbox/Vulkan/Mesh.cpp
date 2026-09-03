#include "Vulkan/Mesh.h"

bool CreateMesh(const VulkanDevice& dev,
                const Commands& commands,
                const MeshDesc& desc,
                const void* vertices, const void* indices,
                Mesh* out) noexcept {
    out->desc = desc;

    const VkDeviceSize vertexBytes =
        VkDeviceSize{desc.vertexLayout.stride} * desc.vertexCount;
    if (!CreateDeviceLocalBuffer(dev, commands, vertices, vertexBytes,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &out->vertices)) {
        return false;
    }

    const VkDeviceSize indexSize =
        desc.indexType == VK_INDEX_TYPE_UINT32 ? sizeof(uint32_t) : sizeof(uint16_t);
    const VkDeviceSize indexBytes = indexSize * desc.indexCount;
    return CreateDeviceLocalBuffer(dev, commands, indices, indexBytes,
                                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &out->indices);
}
