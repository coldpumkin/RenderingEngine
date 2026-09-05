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

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    // Passed through for now. **This is where post-processing goes** -- tone mapping,
    // colour grading and vignette are all edits to this one line.
    //
    // Being a pass-through is also what keeps the two ends apart. Nothing here encodes:
    // the swapchain's sRGB format does that on write, which is why SelectSurfaceFormat
    // refuses a surface without one. Tone-map here and the source has to hold values
    // outside [0,1] -- a float format -- and what is written starts answering to the
    // swapchain's.
    outColor = texture(sceneColor, uv);
}
