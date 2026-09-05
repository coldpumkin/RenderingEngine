#version 450

// scene.vert -- one vertex, object space to clip space
// ============================================================================
//
//   in                                 from                update
//   -------------------------------------------------------------------------
//   inPosition inNormal inTangent inUV vertex buffer       per vertex
//   pc.model pc.normal0..2            push constant       per draw
//   camera.view camera.proj           set 0, binding 0    per frame
//
//   out
//   -------------------------------------------------------------------------
//   gl_Position                       the rasterizer takes it
//   fragWorldPos fragNormal
//   fragTangent fragUV                scene.frag, interpolated on the way
//
// The spaces, and which arrows are this file's:
//
//   object  -> world   pc.model            this file
//   world   -> view    camera.view         this file
//   view    -> clip    camera.proj         this file
//   clip    -> ndc     divide by w         hardware
//   ndc     -> screen  the viewport        hardware
//
// World exists because more than one thing has to meet there. One object alone could
// go object straight to clip; a fragment asking where the camera is, where the light
// is, or where this surface sits in the light's projection cannot. Two things are only
// comparable in a space both are in.
//
// That is also why model rides the draw and the camera's matrices ride the frame:
// model is how to put *this* object into the shared space, and the rest are defined
// *on* that space and belong to no object.

// Per vertex. Ordered by space: an object-space position, two object-space directions,
// and uv, which is a coordinate on the surface and in no space at all.
//
// Contract: offsets and stride match Vertex, laid out by VertexInput() in Vertex.cpp.
//           That half is unchecked -- no compiler reads both. The locations and the
//           kind of number each carries are checked at pipeline creation.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;   // xyz along +u, w = bitangent sign
layout(location = 3) in vec2 inUV;

// Per frame, set 0 binding 0. The front of the block, not all of it: viewPos sits
// behind these and is the fragment stage's. A program's interface is the union of what
// its stages declare, so each names only what it reads.
//
// Contract: view and proj are the first two fields of CameraUniform in Passes.h.
//           Truncating from the front is safe; reordering is not, because a std140
//           offset comes from every field before it.
layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
} camera;

// Per draw. The same truncation rule as the block above.
//
// Contract: these are the front of PushConstants in Passes.h, in order.
layout(push_constant) uniform Push {
    mat4 model;

    // transpose(inverse(mat3(model))), one column per xyz. Three vec4 and not a mat3
    // because GLSL pads a mat3's columns to 16 bytes and glm::mat3 does not.
    vec4 normal0;
    vec4 normal1;
    vec4 normal2;
} pc;

// The same four, one space further along. uv is handed over untouched.
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragTangent;
layout(location = 3) out vec2 fragUV;

void main() {
    // World first: the fragment stage needs the surface point, and a clip position
    // cannot be turned back into one.
    const vec4 world = pc.model * vec4(inPosition, 1.0);
    gl_Position = camera.proj * (camera.view * world);
    fragWorldPos = world.xyz;

    // The normal matrix, not the model matrix. A normal has to stay perpendicular to
    // the surface, and only the inverse transpose keeps it there -- under a non-uniform
    // scale the two matrices give different answers.
    fragNormal = mat3(pc.normal0.xyz, pc.normal1.xyz, pc.normal2.xyz) * inNormal;

    // The model matrix, and mat3 of it so no translation applies. A tangent lies along
    // the surface rather than across it, so it follows the surface itself -- the
    // opposite rule to the normal above.
    //
    // w is a sign and must not be transformed, which is why the two travel as one vec4
    // rather than as a TBN matrix built here. The fragment stage builds TBN instead,
    // after interpolation, which a matrix would not survive.
    fragTangent = vec4(mat3(pc.model) * inTangent.xyz, inTangent.w);
    fragUV = inUV;
}
