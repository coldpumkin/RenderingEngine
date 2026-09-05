#version 450

// shadow.vert -- one vertex, object space to the light's clip space
// ============================================================================
//
//   in                    from                update
//   -------------------------------------------------------------------------
//   inPosition            vertex buffer       per vertex
//   pc.model              push constant       per draw
//   shadow.lightView
//   shadow.lightProj      set 0, binding 0    per frame
//
//   out
//   -------------------------------------------------------------------------
//   gl_Position           the rasterizer takes it. **No varyings** -- nothing runs
//                         after the rasterizer here
//
// The same chain scene.vert walks, with one arrow different -- the observer is the
// light rather than the camera. Depth is the whole product, written by the
// fixed-function test from gl_Position, so this program has no fragment stage.

// Only position. The layout this pipeline is built with declares one attribute over
// the same stride the scene's mesh uses, so both passes walk one vertex buffer and
// this one steps over the rest.
layout(location = 0) in vec3 inPosition;

// The light as a viewpoint. The same buffer scene.frag reads at binding 2, and the
// whole of it here.
//
// Not the same set as the scene's: that one also carries a camera, a shadow map and
// six switches. One layout answering to half of each pass is how a set stops meaning
// anything, and two layouts naming one buffer is what a descriptor already is.
//
// Contract: matches ShadowUniform in Passes.h.
layout(set = 0, binding = 0) uniform Shadow {
    mat4 lightView;
    mat4 lightProj;
} shadow;

// Contract: the first field of PushConstants in Passes.h. A stage may declare part of
//           a block, but the offsets of what it declares have to match -- which is why
//           model is first in both this file and scene.vert.
layout(push_constant) uniform Push {
    mat4 model;
} pc;

void main() {
    gl_Position = shadow.lightProj * (shadow.lightView * (pc.model * vec4(inPosition, 1.0)));
}
