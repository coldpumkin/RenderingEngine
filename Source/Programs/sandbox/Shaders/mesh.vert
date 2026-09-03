#version 450

// Contract: locations and formats match Vertex and the attribute list in Pipeline.h.
// No compiler reads both sides.
layout(location = 0) in vec3 inPosition;   // world space
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;   // xyz along +u, w = bitangent sign

// Set 0 is the frame's: one camera and one light for every draw in the pass.
// Contract: same fields as SceneUniform in Passes.h. Written once per frame.
layout(set = 0, binding = 0) uniform Scene {
    mat4 viewProj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 viewPos;
} scene;

// Contract: same block in the fragment stage, field for field.
layout(push_constant) uniform Push {
    mat4 model;
    float alpha;
    float alphaCutoff;   // read by the fragment stage only, declared here to match
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

    // mat3 is enough while model is rotation and uniform scale only; non-uniform
    // scale needs a normal matrix.
    fragNormal = mat3(pc.model) * inNormal;

    // The same matrix as the normal: a tangent is a direction along the surface, so
    // it rotates with the model. w is a sign, not a direction -- it must not be
    // transformed, which is why the two travel as one vec4 rather than a mat3 built
    // here. Building TBN in the fragment stage also keeps it right after
    // interpolation, which a matrix would not survive.
    fragTangent = vec4(mat3(pc.model) * inTangent.xyz, inTangent.w);
    fragUV = inUV;
}
