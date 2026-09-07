#version 450

// pointshadow.vert -- one vertex, object space to one face of a point light's cube
// ============================================================================
//
// The same walk shadow.vert makes, with two indices instead of one: which light, and
// which of its six faces. Both arrive in the push block, and the pass writes them once
// per face rather than per draw.

layout(location = 0) in vec3 inPosition;

// Contract: matches PointShadowUniform in Passes.h. Light i owns faces 6i .. 6i+5.
layout(set = 0, binding = 0) uniform PointShadow {
    mat4 faceViewProj[24];
    vec4 lightPosRange[4];
} pointShadow;

// Contract: model matches PushConstants in Passes.h; light and face match
//           PointShadowWhich at offsets 64 and 68.
layout(push_constant) uniform Push {
    mat4 model;
    int light;
    int face;
} pc;

// Where this vertex is in the world. The fragment stage needs it to measure how far the
// surface is from the light, which is the whole of what this map stores.
layout(location = 0) out vec3 outWorldPos;

void main() {
    const vec4 world = pc.model * vec4(inPosition, 1.0);
    outWorldPos = world.xyz;
    gl_Position = pointShadow.faceViewProj[pc.light * 6 + pc.face] * world;
}
