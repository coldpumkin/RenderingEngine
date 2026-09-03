#version 450

// Contract: offsets and stride match Vertex, whose VertexInput() in Vertex.cpp is the
//           other side. That half is unchecked -- no compiler reads both.
//           The locations and the kind of number each carries are checked, at pipeline
//           creation, against this file's own SPIR-V.
layout(location = 0) in vec3 inPosition;   // object space. pc.model puts it in world
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;   // xyz along +u, w = bitangent sign

// Set 0 is the frame's: one camera and one light for every draw in the pass.
// Contract: same fields as SceneUniform in Passes.h. Written once per frame.
layout(set = 0, binding = 0) uniform Scene {
    mat4 viewProj;

    // The same world, seen from the light. Here rather than in the push block for the
    // reason the camera is: one light for every draw in the pass.
    mat4 lightViewProj;

    vec4 lightDir;
    vec4 lightColor;
    vec4 viewPos;
    float useNormalMap;
    float useBaseColor;
    float useSpecular;
    float useAlphaMask;
    float useShadow;
} scene;

// Contract: same block in the fragment stage, field for field.
layout(push_constant) uniform Push {
    mat4 model;

    // transpose(inverse(mat3(model))), a column in each xyz. Three vec4 rather than a
    // mat3 because that is what the C++ side can lay out to match: GLSL pads a mat3's
    // columns to 16 bytes and glm::mat3 does not.
    vec4 normal0;
    vec4 normal1;
    vec4 normal2;

    float alpha;   // read by the fragment stage only, declared here to match
} pc;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec4 fragTangent;   // w carried through untouched

void main() {
    // World first, because specular needs the surface point and the clip position
    // cannot be turned back into one.
    const vec4 world = pc.model * vec4(inPosition, 1.0);
    gl_Position = scene.viewProj * world;
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
