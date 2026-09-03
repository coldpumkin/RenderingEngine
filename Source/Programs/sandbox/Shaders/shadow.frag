#version 450

// No outputs, and that is the point of this file.
//
// The pass has no colour attachment: depth is its whole product, written by the
// fixed-function test from gl_Position. This is the first shader here that writes
// nothing, which is what tells CheckOutputInterface that "one colour output" was a
// count and not a rule.
//
// An empty stage rather than no stage: Vulkan allows a pipeline without a fragment
// shader, but a ShaderProgram here is a pair, and a pair is what a set layout and a
// push range are built from.
void main() {}
