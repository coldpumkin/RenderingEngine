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
    float pad0;
    float pad1;
    float pad2;
    vec4 sun;      // xyz = from a surface toward the sun, w unused
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

    // A day sky: deep blue overhead, bright at the horizon, dark ground below it.
    //
    // **The values go above 1.** The cube is 16-bit float and this is radiance, not a
    // colour on a screen -- an environment clamped to 1 lights everything as if the
    // sky were as bright as a sheet of paper, and the irradiance convolved out of it
    // would be flat. The horizon is the brightest part of a clear sky, which is why it
    // is the one over 1 here.
    const vec3 zenith  = vec3(0.18, 0.42, 1.00);
    const vec3 horizon = vec3(1.35, 1.55, 1.85);
    const vec3 ground  = vec3(0.05, 0.043, 0.035);

    const float height = dir.y;
    vec3 sky = mix(horizon, zenith, clamp(pow(max(height, 0.0), 0.55), 0.0, 1.0));

    // A soft edge and not a hard one: the ground is a stand-in for everything below the
    // horizon, and a step there reads as a seam in anything convolved from this.
    sky = mix(ground, sky, smoothstep(-0.12, 0.02, height));

    // The sun, in the direction the scene's light comes from. Two lobes: a small bright
    // disc, and a wide soft glow around it.
    //
    // **This is why the sun direction is a scene fact and not a per-frame value.** What
    // is here is what the irradiance and the prefiltered cube are convolved from, so a
    // sun that moved would leave both of them describing a sky that is no longer there.
    // Moving it means baking all three again, which is the same image turning from an
    // input of the frame into something the frame produces.
    const float alignment = max(dot(dir, normalize(face.sun.xyz)), 0.0);
    const vec3 sunColour = vec3(1.0, 0.94, 0.86);
    sky += sunColour * 220.0 * smoothstep(0.9985, 0.9995, alignment);
    sky += sunColour * 3.0 * pow(alignment, 64.0);

    // Linear light, like every other colour written in this renderer. The swapchain's
    // sRGB format is what encodes at the end, and nothing on the way encodes twice.
    outColor = vec4(sky, 1.0);
}
