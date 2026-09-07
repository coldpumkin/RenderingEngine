#version 450

// The other half of the split-sum: what the BRDF does to whatever the environment sends
//
// A scale and a bias on the surface's reflectance at normal incidence, as a function of
// how head-on the view is and how rough the surface is. Two inputs, one output pair --
// which is a 2D texture, and one that depends on no scene at all. The same numbers for
// every environment and every material, computed once and never again.
//
// Nothing here samples anything. It is the integral of the geometry and Fresnel terms
// over the same GGX lobe the prefilter used, which is what makes the two halves
// multiply back into the whole.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec2 outScaleBias;

const float kPi = 3.14159265359;

vec2 Hammersley(uint i, uint n) {
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return vec2(float(i) / float(n), float(bits) * 2.3283064365386963e-10);
}

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

// Smith's geometry term, with the k the image-based case uses -- half the direct-light
// one, which is the difference between a single light and a whole environment.
float GeometrySmith(float ndotv, float ndotl, float roughness) {
    const float a = roughness;
    const float k = (a * a) / 2.0;
    const float gv = ndotv / (ndotv * (1.0 - k) + k);
    const float gl = ndotl / (ndotl * (1.0 - k) + k);
    return gv * gl;
}

void main() {
    // x is how head-on the view is, y is roughness. Both from the uv directly, which is
    // what makes this table the same shape as the thing it stands for.
    const float ndotv = max(uv.x, 0.001);
    const float roughness = uv.y;

    // Any view with that ndotv will do: the integral is rotationally symmetric about
    // the normal, so only the angle matters.
    const vec3 view = vec3(sqrt(1.0 - ndotv * ndotv), 0.0, ndotv);
    const vec3 normal = vec3(0.0, 0.0, 1.0);

    const uint kSamples = 512u;
    float scale = 0.0;
    float bias = 0.0;

    for (uint i = 0u; i < kSamples; ++i) {
        const vec3 h = ImportanceGGX(Hammersley(i, kSamples), normal, roughness);
        const vec3 l = normalize(2.0 * dot(view, h) * h - view);

        const float ndotl = l.z;
        if (ndotl > 0.0) {
            const float ndoth = max(h.z, 0.0);
            const float vdoth = max(dot(view, h), 0.0);

            const float g = GeometrySmith(ndotv, ndotl, roughness);
            const float gvis = (g * vdoth) / max(ndoth * ndotv, 0.001);

            // Fresnel split so that F0 factors out of the integral entirely -- which is
            // the trick that lets one table serve every material.
            const float fc = pow(1.0 - vdoth, 5.0);
            scale += (1.0 - fc) * gvis;
            bias += fc * gvis;
        }
    }

    outScaleBias = vec2(scale, bias) / float(kSamples);
}
