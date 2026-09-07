#version 450

// bloom_extract.frag -- the part of the picture that is brighter than the display
// ============================================================================
//
// Only useful because the scene chain is a float. On an 8-bit target every bright thing
// is already 1.0, so a threshold would cut along a line the renderer drew rather than
// along one the lighting did.
//
// A soft knee rather than a step: a hard threshold makes a moving highlight pop in and
// out as it crosses it, and the seam is visible on a slowly panning camera.

layout(set = 0, binding = 0) uniform sampler2D sceneColor;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

const float kThreshold = 1.0;   // where the display runs out
const float kKnee = 0.6;        // how wide the fade into it is

void main() {
    // Four taps rather than one. This runs at half the scene's size, so a single sample
    // would drop three quarters of the pixels and a bright pixel could fall between the
    // gaps and flicker as the camera moves.
    const vec2 texel = 1.0 / vec2(textureSize(sceneColor, 0));
    vec3 colour = texture(sceneColor, uv + texel * vec2(-0.5, -0.5)).rgb;
    colour += texture(sceneColor, uv + texel * vec2( 0.5, -0.5)).rgb;
    colour += texture(sceneColor, uv + texel * vec2(-0.5,  0.5)).rgb;
    colour += texture(sceneColor, uv + texel * vec2( 0.5,  0.5)).rgb;
    colour *= 0.25;

    // The brightest channel decides, so a saturated red highlight blooms as much as a
    // white one of the same intensity.
    const float brightest = max(colour.r, max(colour.g, colour.b));
    const float weight = smoothstep(kThreshold - kKnee, kThreshold + kKnee, brightest);

    outColor = vec4(colour * weight, 1.0);
}
