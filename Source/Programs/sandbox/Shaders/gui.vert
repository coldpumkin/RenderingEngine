#version 450

// The panel's vertices, which ImGui produces and we draw. A second vertex type, so
// a second layout -- see GuiVertexInput in Gui.cpp.
//
// Contract: matches ImDrawVert. pos and uv are floats, col is four bytes read as
//           UNORM, which is why the shader sees 0..1 and not 0..255.
layout(location = 0) in vec2 inPos;     // window pixels, origin top-left
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

// No camera and no model matrix: the panel is already in pixels, and this turns
// pixels into clip space. Two vec2 rather than a mat4 because that is all an
// orthographic screen-space transform is, and 16 bytes beats 64.
layout(push_constant) uniform Push {
    vec2 scale;       //  2 / framebuffer size
    vec2 translate;   // -1
} pc;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main() {
    fragUV = inUV;
    fragColor = inColor;
    gl_Position = vec4(inPos * pc.scale + pc.translate, 0.0, 1.0);
}
