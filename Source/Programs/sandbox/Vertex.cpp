#include "Vertex.h"

#include <iterator>   // std::size

// The vertex layout, derived entirely from Vertex: stride, offsets and formats all
// come from the struct, so a field change cannot desync them. A second vertex type
// gets its own pair beside this one.
//
// A narrower format fills the rest silently - a vec2 here feeds a vec3 with z = 0.
// One entry per attribute the shader reads; the layer warns about any extra.
const VkPipelineVertexInputStateCreateInfo& VertexInput() noexcept {
    static constexpr VkVertexInputBindingDescription binding{
        0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};

    static constexpr VkVertexInputAttributeDescription attributes[]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, uv)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
    };

    static const VkPipelineVertexInputStateCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
        1, &binding,
        static_cast<uint32_t>(std::size(attributes)), attributes};
    return info;
}
