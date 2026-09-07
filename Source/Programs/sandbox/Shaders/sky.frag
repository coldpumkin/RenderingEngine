#version 450

// The sky, drawn where nothing else will be
//
// Runs before whatever fills the middle, with no depth attachment and no depth test:
// every pixel is written and the geometry drawn afterwards covers what it covers. The
// cost is drawing a pixel twice where geometry lands; the alternative is testing depth,
// which needs the middle's depth to have been written and stored first and is a
// different arrangement of the frame.
//
// Contract: set 0 is the frame set (Passes.h). This stage reads binding 0 and binding
//           5; the four between them are holes in this program's layout, which is what
//           lets one FrameSetSources fill every program that uses the set.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform Camera {
    mat4 view;
    mat4 proj;
    vec4 viewPos;
} camera;

layout(set = 0, binding = 5) uniform samplerCube skyCube;

void main() {
    // The direction this pixel looks along, which is the whole of what a cube map is
    // addressed by. Depth 1.0 is the far plane, so this is where the ray leaves.
    const vec4 ndc = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    const vec4 viewSpace = inverse(camera.proj) * ndc;

    // w-divide first, then out of view space with the rotation only: a direction has no
    // position, so the translation in the inverse view is not applied to it.
    const vec3 viewDir = normalize(viewSpace.xyz / viewSpace.w);
    const vec3 world = normalize(mat3(inverse(camera.view)) * viewDir);

    outColor = vec4(texture(skyCube, world).rgb, 1.0);
}
