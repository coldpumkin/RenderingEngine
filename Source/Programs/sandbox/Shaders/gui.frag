#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

// One image for the whole panel: the font atlas. Text, the checkbox marks and the
// window backgrounds all read from it -- the solid parts use a white texel ImGui
// reserves for exactly that, so a filled rectangle and a letter are the same draw.
//
// Set 0, because there is only one set here and Vulkan numbers from zero. It has
// nothing to do with the scene's set 0; a pipeline layout is per pipeline.
layout(set = 0, binding = 0) uniform sampler2D fontAtlas;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = fragColor * texture(fontAtlas, fragUV);
}
