#version 450

// fullscreen.vert -- three points that cover the screen, from nothing
//
//   in    gl_VertexIndex, and nothing else. **No vertex buffer**, which is why this
//         pipeline's vertex layout has stride 0
//   out   gl_Position to the rasterizer, uv to the fragment stage
//
// Paired with post.frag today; a lighting stage would pair this same module with a
// different fragment shader.
//
// One triangle larger than the screen rather than a quad of two. The scissor clips the
// overhang so the result is the same, and a quad shades the pixels along the diagonal
// where its two triangles meet twice.
//
//   index 0 -> uv(0,0) -> pos(-1,-1)
//   index 1 -> uv(2,0) -> pos( 3,-1)
//   index 2 -> uv(0,2) -> pos(-1, 3)
layout(location = 0) out vec2 uv;

void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
