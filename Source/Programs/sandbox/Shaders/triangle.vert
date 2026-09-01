#version 450

// Contract: locations and formats match Vertex and the attribute list in
// CreateTrianglePipeline (Pipeline.h). No compiler reads both sides.
layout(location = 0) in vec3 inPosition;   // world space
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// Contract: same block in the fragment stage, field for field.
layout(push_constant) uniform Push {
    mat4 mvp;
    float alpha;
} pc;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUV;

void main() {
    // The rasterizer does the perspective divide after this.
    gl_Position = pc.mvp * vec4(inPosition, 1.0);

    // Object space: mvp arrives premultiplied. Lighting wants world space, which is
    // where model splits out of mvp.
    fragNormal = inNormal;
    fragUV = inUV;
}
