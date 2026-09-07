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

// Output: the clip-space xy a fullscreen uv stands for
//
// **Not uv * 2 - 1.** The passes that draw geometry use a negative viewport height
// (ViewportY::Up), which puts clip y = +1 at the top of the framebuffer; a fullscreen
// triangle is drawn without that flip, so its uv.y is 0 at the top. Anything undoing
// the camera's projection has to read the first convention out of a uv written in the
// second, and the y sign is the whole of the difference.
//
// Getting it wrong is quiet: the picture still fills the screen, and what moves is
// where the shader thinks each pixel is looking.
vec2 ClipFromUv(vec2 uv) {
    return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

void main() {
    // The direction this pixel looks along, which is the whole of what a cube map is
    // addressed by. Depth 1.0 is the far plane, so this is where the ray leaves.
    const vec4 ndc = vec4(ClipFromUv(uv), 1.0, 1.0);
    const vec4 viewSpace = inverse(camera.proj) * ndc;

    // w-divide first, then out of view space with the rotation only: a direction has no
    // position, so the translation in the inverse view is not applied to it.
    const vec3 viewDir = normalize(viewSpace.xyz / viewSpace.w);
    const vec3 world = normalize(mat3(inverse(camera.view)) * viewDir);

    outColor = vec4(texture(skyCube, world).rgb, 1.0);
}
