#version 450

// One face of one mip of the prefiltered environment
//
// Half of the split-sum approximation. The specular integral depends on the normal, the
// view direction and the roughness, which is one dimension too many for a cube map --
// so this drops the view by assuming it equals the normal, which is exact head-on and
// the reason a grazing reflection off a rough surface is the one thing this gets wrong.
//
// Roughness lives in the mip chain: level 0 is a mirror and each level is blurrier, so
// a shader picks a level from the roughness and the hardware interpolates between them.
// That is why this is drawn once per level rather than once per face, and why
// ImageViewDesc's baseMip exists.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

// Contract: matches PrefilterFace in Sky.h. The face, and how rough this level is --
//           the level number itself never reaches the shader, only what it means.
layout(push_constant) uniform Face {
    int index;
    float roughness;
} face;

layout(set = 0, binding = 0) uniform samplerCube environment;

// Contract: the same six as skybake.frag.
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

// Output: the i-th of n points spread over the unit square, low-discrepancy
//
// Hammersley: one coordinate is the index, the other is that index with its bits
// reversed. Cheap, and it fills the square more evenly than random pairs, which is what
// keeps the sample count low enough to run at startup.
vec2 Hammersley(uint i, uint n) {
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return vec2(float(i) / float(n), float(bits) * 2.3283064365386963e-10);
}

// Output: a half-vector drawn from the GGX distribution around normal
//
// Importance sampling: the samples go where the lobe actually is, so a rough surface
// does not need thousands of them to stop looking speckled.
vec3 ImportanceGGX(vec2 xi, vec3 normal, float roughness) {
    const float a = roughness * roughness;

    const float phi = 2.0 * kPi * xi.x;
    const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    const vec3 h = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    const vec3 hint = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tangent = normalize(cross(hint, normal));
    const vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * h.x + bitangent * h.y + normal * h.z);
}

void main() {
    const vec3 normal = normalize(DirectionFor(face.index, uv));

    // The assumption this whole map rests on, written where it is made.
    const vec3 view = normal;

    const uint kSamples = 128u;
    vec3 sum = vec3(0.0);
    float weight = 0.0;

    for (uint i = 0u; i < kSamples; ++i) {
        const vec3 h = ImportanceGGX(Hammersley(i, kSamples), normal, face.roughness);
        const vec3 l = normalize(2.0 * dot(view, h) * h - view);

        // Samples pointing away from the surface contribute nothing and are not counted,
        // which is what keeps the average an average.
        const float ndotl = dot(normal, l);
        if (ndotl > 0.0) {
            sum += texture(environment, l).rgb * ndotl;
            weight += ndotl;
        }
    }

    outColor = vec4(sum / max(weight, 0.001), 1.0);
}
