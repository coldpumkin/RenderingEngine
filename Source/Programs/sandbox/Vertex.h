#pragma once

// Vertex - what one of our vertices is, and how a pipeline reads it
// ============================================================================
//
// Outside Vulkan/ because these fields answer to scene.vert's locations, not to the
// API. Mesh takes a stride and stays layout-agnostic, so this is the only place that
// knows which bytes are position.
//
// Appending never moves an earlier offset, so a new field cannot disturb a shader.
//
// Stride 48, all float so no padding:
//
//   position  12   object space -- the push constant's model puts it in world
//   normal    12   +z for a z=0 face wound CCW in y-up
//   uv         8   (0,0) top-left, y down
//   tangent   16   xyz, w = bitangent sign (glTF TANGENT)
//
// Contract: a field is not an attribute. VertexInput() declares what the buffer
//           carries; a shader reads the locations it needs and the pipeline layer
//           checks those, not the count.

#include "Vulkan/VertexLayout.h"

struct Vertex {
    float position[3];   // location 0
    float normal[3];     // location 1
    float uv[2];         // location 2
    float tangent[4];    // location 3
};

// The vertex layout for Vertex, as a value. Callers put it in desc.vertexLayout; a
// shader that builds its own points (fullscreen) leaves that default, stride 0.
//
// One of these, not one per shader. A depth-only pass reading position alone is built
// from this same value: which locations a pipeline actually consumes is decided by
// its vertex stage, and the .spv already says so. A second "position only" layout
// would be that answer written out by hand, and it would be the hand-written copy
// that goes stale.
VertexLayout VertexInput() noexcept;
