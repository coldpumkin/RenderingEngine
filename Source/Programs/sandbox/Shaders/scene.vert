#version 450

// The spaces a vertex passes through, and which arrow this file owns
// ============================================================================
//
//   object    what the file says. Vertex.position, straight out of the glTF
//   world     pc.model puts it here. **The only space where things can be compared**
//   view      camera.view takes it here -- the camera at the origin looking down -z
//   clip      camera.proj takes it there, and gl_Position is where it leaves
//   ndc       clip / w, done by the hardware
//   screen    the viewport decides, and its sign is RasterState::viewportY
//
// This stage owns three arrows of that chain -- object to world, world to view, view
// to clip. Everything after gl_Position belongs to the hardware.
//
// The last two are two matrices and not their product because they answer to different
// things: view to where the camera is, proj to the size of the target this lands on.
// Passes.h says the rest.
//
// **World exists because more than one thing has to meet.** Drawing a single object
// needs none of it: object straight to clip would do. It appears the moment a fragment
// asks about something that is not itself, and scene.frag asks three times -- where
// the camera is, where the light is, and where this surface sits in the light's own
// projection. Two things can only be compared in a space both of them are in.
//
// That is also why pc.model rides the draw while the camera's matrices, the light's
// and viewPos sit in the frame's uniform. model is "how to put *this* object into the
// shared space"; the rest are defined *on* that shared space and belong to no object.
//
// uv is not on the chain at all -- it is a coordinate on the surface, and no transform
// here touches it. That is why it sits last on both sides below.

// Contract: offsets and stride match Vertex, whose VertexInput() in Vertex.cpp is the
//           other side. That half is unchecked -- no compiler reads both.
//           The locations and the kind of number each carries are checked, at pipeline
//           creation, against this file's own SPIR-V.
//
// Ordered by space and then by what the value is: an object-space position, two
// object-space directions, and the one that is in no space at all.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;   // xyz along +u, w = bitangent sign
layout(location = 3) in vec2 inUV;

// Set 0 is the frame's, and this stage reads one thing out of it.
//
// The front of the block, not all of it. A program's interface is the union of what
// its stages require, not one declaration copied into each -- scene.frag names the
// rest, and BuildSetLayout ors the two. Declaring fields this stage never reads made
// the file look like it needed a light and four switches to place a vertex.
//
// Truncating is safe where reordering is not: std140 offsets are decided by what
// comes before a field, so the first N fields sit where they sit. Reading a later one
// means saying its offset, the way scene.frag's push block does.
//
// The camera, and only the fields this stage moves a vertex with. viewPos sits behind
// them and is the fragment stage's; truncating from the front is what leaves it out.
//
// Contract: view and proj are the first two fields of CameraUniform in Passes.h.
layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
} camera;

// What this stage reads of the push block, and no more. The fragment stage declares
// its own field at its own offset; the two do not have to look alike, and a program's
// push range is the span both of them together need.
//
// Contract: these fields are the front of PushConstants in Passes.h, in order. Their
//           offsets follow from that -- truncating is safe, reordering is not.
layout(push_constant) uniform Push {
    mat4 model;

    // transpose(inverse(mat3(model))), a column in each xyz. Three vec4 rather than a
    // mat3 because that is what the C++ side can lay out to match: GLSL pads a mat3's
    // columns to 16 bytes and glm::mat3 does not.
    vec4 normal0;
    vec4 normal1;
    vec4 normal2;
} pc;

// The same four in the same order, one space further along. Reading the two lists
// against each other is what this stage does: three values move object to world, and
// uv is handed over untouched.
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragTangent;   // w carried through untouched
layout(location = 3) out vec2 fragUV;

void main() {
    // World first, because specular needs the surface point and the clip position
    // cannot be turned back into one.
    const vec4 world = pc.model * vec4(inPosition, 1.0);
    gl_Position = camera.proj * (camera.view * world);
    fragWorldPos = world.xyz;

    // The normal matrix, not the model matrix. A normal is a covector: it stays
    // perpendicular to the surface only under the inverse transpose, and under a
    // non-uniform scale the two answers differ. Computed once per draw on the CPU.
    fragNormal = mat3(pc.normal0.xyz, pc.normal1.xyz, pc.normal2.xyz) * inNormal;

    // The model matrix, not the normal matrix. A tangent is a direction along the
    // surface, so it transforms like a position does -- the opposite rule to the
    // normal above, and the two only give the same answer while the scale is uniform.
    //
    // w is a sign, not a direction: it must not be transformed, which is why the two
    // travel as one vec4 rather than a mat3 built here. Building TBN in the fragment
    // stage also keeps it right after interpolation, which a matrix would not survive.
    fragTangent = vec4(mat3(pc.model) * inTangent.xyz, inTangent.w);
    fragUV = inUV;
}
