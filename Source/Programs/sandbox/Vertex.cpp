#include "Vertex.h"

// The vertex layout, derived entirely from Vertex: stride, offsets and formats all
// come from the struct, so a field change cannot desync them. A second vertex type
// gets its own pair beside this one.
//
// A narrower format fills the rest silently - a vec2 here feeds a vec3 with z = 0.
// One entry per attribute the shader reads; the layer warns about any extra.
VertexLayout VertexInput() noexcept {
    VertexLayout layout;
    layout.stride = sizeof(Vertex);
    layout.attributeCount = 4;
    layout.attributes[0] = {0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, position)};
    layout.attributes[1] = {1, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, normal)};
    layout.attributes[2] = {2, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)};
    layout.attributes[3] = {3, VK_FORMAT_R32G32_SFLOAT,       offsetof(Vertex, uv)};
    return layout;
}
