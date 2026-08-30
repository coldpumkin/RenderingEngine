#version 450

// **셰이더가 처음으로 이미지를 읽는다.** 지금까지는 정점과 푸시 상수뿐이었다.
//
// set=0, binding=0은 C++의 디스크립터 셋 레이아웃과 짝이 맞아야 한다.
// 어긋나면 검증 레이어가 잡는다.
layout(set = 0, binding = 0) uniform sampler2D sceneColor;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    // 지금은 그대로 통과시킨다. **여기가 후처리가 들어올 자리다** -
    // 톤매핑·색보정·비네트가 전부 이 한 줄을 고치는 일이 된다.
    outColor = texture(sceneColor, uv);
}
