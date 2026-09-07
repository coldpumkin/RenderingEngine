#version 450

// pointshadow.frag -- how far this surface is from the light
// ============================================================================
//
// A distance rather than a depth. A depth would be this face's projection applied to the
// position, and reading it back would mean working out which face a direction landed on
// and undoing that projection. A distance is the same number from every face, so the
// comparison at the other end is one subtraction.
//
// Divided by the light's range, so what is stored is 0..1 and a 16-bit float carries it
// with room to spare.

layout(location = 0) in vec3 inWorldPos;

// Contract: matches PointShadowUniform in Passes.h.
layout(set = 0, binding = 0) uniform PointShadow {
    mat4 faceViewProj[24];
    vec4 lightPosRange[4];
} pointShadow;

// Only the light index. The offset is where PointShadowWhich puts it, and the members in
// front of it are the vertex stage's business.
layout(push_constant) uniform Push {
    layout(offset = 64) int light;
} pc;

layout(location = 0) out float outDistance;

void main() {
    const vec4 posRange = pointShadow.lightPosRange[pc.light];
    outDistance = length(inWorldPos - posRange.xyz) / max(posRange.w, 0.0001);
}
