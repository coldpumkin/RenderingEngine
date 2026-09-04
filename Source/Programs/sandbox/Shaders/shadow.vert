#version 450

// The depth-only pass: where a surface is, seen from the light.
//
// Only position is read. The layout this pipeline is built with declares one
// attribute over the same stride the scene's mesh uses, so both passes walk one
// vertex buffer and this one steps over the rest.
layout(location = 0) in vec3 inPosition;

// The light. The same buffer the scene pass reads, declared here down to the one
// field this stage needs -- the direction and colour behind it are for shading, which
// happens nowhere in this file.
//
// Not the same set: the scene's also carries a camera, a shadow map and six switches,
// and one layout answering to half of each pass is how a set stops meaning anything.
// Two layouts naming one buffer is what a descriptor already is.
//
// Contract: the front of LightUniform in Passes.h. Truncating is safe because
//           lightViewProj is first; reordering that struct breaks this file silently.
layout(set = 0, binding = 0) uniform Light {
    mat4 lightViewProj;
} light;

// Contract: the first field of PushConstants in Passes.h. A stage may declare part of
//           a block, and this one needs no more -- but the offsets of what it does
//           declare have to match, which is why model is first in both.
layout(push_constant) uniform Push {
    mat4 model;
} pc;

void main() {
    gl_Position = light.lightViewProj * (pc.model * vec4(inPosition, 1.0));
}
