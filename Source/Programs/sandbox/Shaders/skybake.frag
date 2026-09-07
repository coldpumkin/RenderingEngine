#version 450

// One face of the sky cube, from a direction this face's texel stands for
//
// Run once at startup, six times. The face index arrives as a push constant because it
// is the only thing that differs between the six, and a direction is built from it and
// the uv rather than looked up: a cube's texel address IS a direction, so the two are
// the same statement written from either end.
//
// The gradient is invented rather than sampled from anything. Nothing here needs an
// asset for the cube map path to be real, and a photograph can replace this function
// without any of the rest of it moving.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

// Contract: matches PushConstants in Sky.h. One int, and nothing about the camera --
//           a face of the cube is the same whichever way anyone is looking.
layout(push_constant) uniform Face {
    int index;
} face;

// Output: the direction this texel of this face stands for, unnormalised
//
// The six faces of a cube map in Vulkan's order: +X -X +Y -Y +Z -Z. The axis signs are
// the convention the sampler uses, so getting one wrong shows up as a seam rather than
// as nothing.
vec3 DirectionFor(int index, vec2 st) {
    // -1..1 across the face, y down, which is how a cube face is laid out.
    const float u = st.x * 2.0 - 1.0;
    const float v = st.y * 2.0 - 1.0;

    switch (index) {
        case 0:  return vec3( 1.0,   -v,   -u);   // +X
        case 1:  return vec3(-1.0,   -v,    u);   // -X
        case 2:  return vec3(   u,  1.0,    v);   // +Y
        case 3:  return vec3(   u, -1.0,   -v);   // -Y
        case 4:  return vec3(   u,   -v,  1.0);   // +Z
        default: return vec3(  -u,   -v, -1.0);   // -Z
    }
}

void main() {
    const vec3 dir = normalize(DirectionFor(face.index, uv));

    // A day sky: deep blue overhead, pale at the horizon, brown below it. The height is
    // the y of the direction, which is what makes this a function of direction alone
    // and so the one thing a cube map can hold.
    const vec3 zenith  = vec3(0.10, 0.26, 0.62);
    const vec3 horizon = vec3(0.62, 0.72, 0.86);
    const vec3 ground  = vec3(0.16, 0.13, 0.10);

    const float height = dir.y;
    vec3 sky = mix(horizon, zenith, clamp(height * 1.6, 0.0, 1.0));
    sky = mix(ground, sky, clamp((height + 0.05) * 12.0, 0.0, 1.0));

    // Linear light, like every other colour written in this renderer. The swapchain's
    // sRGB format is what encodes at the end, and nothing on the way encodes twice.
    outColor = vec4(sky, 1.0);
}
