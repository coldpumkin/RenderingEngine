#version 450

// What the scene pass produced. Set 0 because it is counted the way the scene's set 0
// is -- one per frame in flight, each naming that frame's resolve image.
//
// The set layout is built from this declaration by reflection, so the two cannot
// disagree. What is not checked anywhere is that the image bound here is the one the
// scene pass wrote: nothing names that resource, and CreatePostProcessPass reaches it
// by walking a path.
layout(set = 0, binding = 0) uniform sampler2D sceneColor;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    // Passed through for now. **This is where post-processing goes** -- tone mapping,
    // colour grading and vignette are all edits to this one line.
    //
    // Being a pass-through is also what keeps the two ends of the chain apart. Nothing
    // here encodes: the swapchain's sRGB format does that on write, which is why
    // SelectSurfaceFormat refuses a surface without one. Change this line to tone-map
    // and the source has to hold values outside [0,1] -- a float format -- and what is
    // written here starts answering to the swapchain's.
    outColor = texture(sceneColor, uv);
}
