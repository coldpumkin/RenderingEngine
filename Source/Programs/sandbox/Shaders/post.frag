#version 450

// post.frag -- the scene's image onto the frame's target
//
//   in    uv from fullscreen.vert, interpolated   per fragment
//         sceneColor, set 0 binding 0             per frame
//   out   outColor, location 0
//
// No material set and no push constant: this stage draws no object, so nothing here is
// counted per material or per draw.

// What the scene pass produced. Counted the way the scene's set 0 is -- one per frame
// in flight, each naming that frame's resolve image.
//
// The set layout is built from this declaration by reflection, so the two cannot
// disagree. What nothing checks is that the image bound here is the one the scene pass
// wrote: no name says so, and CreatePostProcessPass reaches it by walking a path.
layout(set = 0, binding = 0) uniform sampler2D sceneColor;

// What the bloom pass left in its first image: the part of the picture that was above
// the display's range, spread out. Half size, so the sampler's own filtering widens it
// once more on the way back.
layout(set = 0, binding = 1) uniform sampler2D bloom;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

// Output: the same picture with its range brought into 0..1
//
// The scene chain is a float, so what arrives here is radiance and the sky's sun is two
// hundred times a lit wall. A clamp would turn every bright thing into the same white;
// this curve compresses the top instead, so a highlight keeps some of its shape.
//
// Narkowicz's fit of the ACES tone curve. Five constants and no branch, which is why it
// is the one most real-time renderers reach for.
// How much of the spread light comes back. Not 1: the extract already kept only what
// was over the range, so adding all of it back would put the same energy in twice.
const float kBloomStrength = 0.35;

vec3 Tonemap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    // Nothing here encodes. The swapchain's sRGB format does that on write, which is
    // why SelectSurfaceFormat refuses a surface without one -- so what this writes is
    // still linear, only compressed into the range the display can hold.
    // Added before the curve rather than after it. Bloom is light that was there and
    // spread; adding it to an already compressed image would brighten what is dark
    // instead of widening what is bright.
    const vec3 lit = texture(sceneColor, uv).rgb
                   + texture(bloom, uv).rgb * kBloomStrength;
    outColor = vec4(Tonemap(lit), 1.0);
}
