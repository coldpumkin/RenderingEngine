#version 450

// Contract: locations and formats match Vertex and the attribute list in Pipeline.h.
// No compiler reads both sides.
layout(location = 0) in vec3 inPosition;   // world space
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

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
} pc;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 fragWorldPos;

void main() {
    // World first, because specular needs the surface point and the clip position
    // cannot be turned back into one.
    const vec4 world = pc.model * vec4(inPosition, 1.0);
    gl_Position = scene.viewProj * world;
    fragWorldPos = world.xyz;

    // mat3 is enough while model is rotation and uniform scale only; non-uniform
    // scale needs a normal matrix.
    fragNormal = mat3(pc.model) * inNormal;
    fragUV = inUV;
}
