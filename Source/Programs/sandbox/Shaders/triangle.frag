#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;

// Contract: one sampler2D here, one binding in sceneLayout. Nothing reads both.
layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    float alpha;
} pc;

void main() {
    // Normal as color until lighting exists: a z=0 face must come out blue.
    const vec3 base = normalize(fragNormal) * 0.5 + 0.5;
    outColor = vec4(base * texture(tex, fragUV).rgb, pc.alpha);
}
