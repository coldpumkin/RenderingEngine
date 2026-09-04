#version 450

// The depth-only pass: where a surface is, seen from the light.
//
// Only position is read. The layout this pipeline is built with declares one
// attribute over the same stride the scene's mesh uses, so both passes walk one
// vertex buffer and this one steps over the rest.
layout(location = 0) in vec3 inPosition;

// The matrix that puts a surface where this pass looks from. The same buffer the
// scene pass reads, and the whole of it: the light's direction and colour are for
// shading, which happens nowhere in this file, and they are a block of their own.
//
// Not the same set: the scene's also carries a camera, a shadow map and six switches,
// and one layout answering to half of each pass is how a set stops meaning anything.
// Two layouts naming one buffer is what a descriptor already is.
//
// Contract: matches ShadowUniform in Passes.h.
layout(set = 0, binding = 0) uniform Shadow {
    mat4 lightView;
    mat4 lightProj;
} shadow;

// Contract: the first field of PushConstants in Passes.h. A stage may declare part of
//           a block, and this one needs no more -- but the offsets of what it does
//           declare have to match, which is why model is first in both.
layout(push_constant) uniform Push {
    mat4 model;
} pc;

void main() {
    gl_Position = shadow.lightProj * (shadow.lightView * (pc.model * vec4(inPosition, 1.0)));
}
