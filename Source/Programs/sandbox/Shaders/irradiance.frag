#version 450

// One face of the diffuse irradiance cube, convolved from the sky
//
// What a matte surface facing a direction receives from the whole environment: the
// hemisphere around that direction, each sample weighted by the cosine of its angle to
// it. That integral has no dependence on where the surface is or which way the eye is,
// which is the whole reason it can be a cube map at all -- it is a function of the
// normal and nothing else.
//
// Run once, at startup, into a small cube. 32 a side is the usual size and is not
// stinginess: the result is smooth by construction, so resolution buys nothing.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

// Contract: matches SkyFace in Sky.h -- the whole of it, because one loop bakes both
//           cubes and pushes one struct. The sun is the sky's and is read there; a
//           block that declared only the face would be a four-byte range receiving
//           thirty-two.
layout(push_constant) uniform Face {
    int index;
    float pad0;
    float pad1;
    float pad2;
    vec4 sun;
} face;

layout(set = 0, binding = 0) uniform samplerCube environment;

// Contract: the same six as skybake.frag. Written twice because the two shaders share
//           no code path, and a mismatch shows as a rotated environment.
vec3 DirectionFor(int index, vec2 st) {
    const float u = st.x * 2.0 - 1.0;
    const float v = st.y * 2.0 - 1.0;
    switch (index) {
        case 0:  return vec3( 1.0,   -v,   -u);
        case 1:  return vec3(-1.0,   -v,    u);
        case 2:  return vec3(   u,  1.0,    v);
        case 3:  return vec3(   u, -1.0,   -v);
        case 4:  return vec3(   u,   -v,  1.0);
        default: return vec3(  -u,   -v, -1.0);
    }
}

const float kPi = 3.14159265359;

void main() {
    const vec3 normal = normalize(DirectionFor(face.index, uv));

    // A frame around the normal. The up hint is swapped near the poles for the reason
    // every lookAt in this repo does it: two nearly parallel vectors have no cross.
    const vec3 hint = abs(normal.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    const vec3 right = normalize(cross(hint, normal));
    const vec3 up = cross(normal, right);

    // Marching phi and theta rather than importance sampling: this runs once and the
    // integrand is smooth, so the simple sum is both enough and easier to read.
    const float step = 0.025;
    vec3 sum = vec3(0.0);
    float samples = 0.0;

    for (float phi = 0.0; phi < 2.0 * kPi; phi += step) {
        for (float theta = 0.0; theta < 0.5 * kPi; theta += step) {
            const vec3 tangent = vec3(sin(theta) * cos(phi),
                                      sin(theta) * sin(phi),
                                      cos(theta));
            const vec3 world = tangent.x * right + tangent.y * up + tangent.z * normal;

            // cos weights the contribution and sin is the area of the ring this sample
            // stands for -- together they are what makes the sum an integral.
            sum += texture(environment, world).rgb * cos(theta) * sin(theta);
            samples += 1.0;
        }
    }

    outColor = vec4(kPi * sum / max(samples, 1.0), 1.0);
}
