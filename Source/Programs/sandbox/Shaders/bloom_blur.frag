#version 450

// bloom_blur.frag -- one axis of a Gaussian blur
// ============================================================================
//
// Run twice, horizontally and then vertically. A 2D Gaussian is the product of two 1D
// ones, so two passes of N taps cost 2N samples where one pass of the same width costs
// N squared -- at nine taps that is 18 against 81.
//
// The step is a push constant because the two runs differ in nothing else: same shader,
// same pipeline, and only which way it walks changes.

layout(set = 0, binding = 0) uniform sampler2D source;

// Contract: blurStep is the first member of the push block, where a draw's model matrix
//           sits in the programs that draw geometry. The two never appear in one
//           program, which is the same arrangement the shadow passes' light index uses.
layout(push_constant) uniform Push {
    vec2 blurStep;   // one texel along the axis being blurred, in uv
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

// Nine taps, sigma about two texels. Weights are the binomial row, which is what a
// Gaussian comes to on a small integer grid.
const float kWeights[5] = float[](0.227027, 0.194594, 0.121621, 0.054054, 0.016216);

void main() {
    vec3 sum = texture(source, uv).rgb * kWeights[0];
    for (int i = 1; i < 5; ++i) {
        const vec2 offset = pc.blurStep * float(i);
        sum += texture(source, uv + offset).rgb * kWeights[i];
        sum += texture(source, uv - offset).rgb * kWeights[i];
    }
    outColor = vec4(sum, 1.0);
}
