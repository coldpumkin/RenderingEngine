#version 450

// 정점에서 넘어온 색을 그대로 쓴다. 래스터라이저가 세 정점 사이를 보간해준다.
layout(location = 0) in vec3 fragColor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}
