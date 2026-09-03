#version 450

// The depth-only pass: where a surface is, seen from the light.
//
// Only position is read. The layout this pipeline is built with declares one
// attribute over the same stride the scene's mesh uses, so both passes walk one
// vertex buffer and this one steps over the rest.
layout(location = 0) in vec3 inPosition;

// Set 0, counted per frame in flight like the scene pass's -- and not the same set.
// That one carries a camera, a light and four switches this stage has no use for, and
// sharing it would mean one layout that only half of each pass answers to.
//
// Contract: same field as ShadowUniform in Passes.h.
layout(set = 0, binding = 0) uniform Shadow {
    mat4 lightViewProj;
} shadow;

// Contract: the first field of PushConstants in Passes.h. A stage may declare part of
//           a block, and this one needs no more -- but the offsets of what it does
//           declare have to match, which is why model is first in both.
layout(push_constant) uniform Push {
    mat4 model;
} pc;

void main() {
    gl_Position = shadow.lightViewProj * (pc.model * vec4(inPosition, 1.0));
}
