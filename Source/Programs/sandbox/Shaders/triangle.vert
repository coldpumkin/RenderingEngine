#version 450

// **정점 버퍼에서 읽는다.** 전에는 gl_VertexIndex로 상수 배열에서 꺼냈다.
//
// location 번호는 C++의 VkVertexInputAttributeDescription::location과 짝이 맞아야 한다.
// 어긋나면 컴파일도 되고 실행도 되는데 화면만 이상해진다 - 검증 레이어가 잡아주는
// 몇 안 되는 경우이기도 하다.
//
// **좌표 규약: y가 위로 향한다.** Vulkan 기본 NDC는 y가 아래로 향하지만, C++ 쪽에서
// 뷰포트 height를 음수로 줘서 뒤집어놨다 (GLM 같은 수학 라이브러리가 y-up을 가정하고
// 대부분의 엔진이 같은 선택을 한다). 정점 데이터도 y-up으로 적는다.
//
// **z가 생겼다.** 전에는 여기서 0.0을 박았다 - 모든 것이 같은 깊이라 뎁스 테스트가
// 의미가 없었다. 이제 정점이 자기 깊이를 들고 온다.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition, 1.0);
    fragColor = inColor;
}
