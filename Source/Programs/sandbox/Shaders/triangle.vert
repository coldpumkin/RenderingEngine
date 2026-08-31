#version 450

// **정점 버퍼에서 읽는다.** 전에는 gl_VertexIndex로 상수 배열에서 꺼냈다.
//
// location 번호는 C++의 VkVertexInputAttributeDescription::location과 짝이 맞아야 한다.
// 어긋나면 컴파일도 되고 실행도 되는데 화면만 이상해진다 - 검증 레이어가 잡아주는
// 몇 안 되는 경우이기도 하다.
//
// **좌표 규약: y가 위로 향한다.** Vulkan 기본 NDC는 y가 아래로 향하지만, C++ 쪽에서
// 뷰포트 height를 음수로 줘서 뒤집어놨다. GLM도 y-up을 가정하므로 둘이 맞는다 -
// **proj[1][1] *= -1을 같이 하면 이중 반전이라 화면이 뒤집힌다.**
//
// **inPosition이 이제 world 좌표다.** 전에는 NDC를 직접 적었다 - 그건 "화면 어디"지
// "어디에 있는가"가 아니었고, 그래서 물체를 하나 더 놓을 자리가 없었다.
// 화면 위치는 아래 mvp가 정한다.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

// C++의 PushConstants와 필드 순서·타입이 정확히 같아야 한다 (Pipeline.h).
// GLSL의 mat4도 column-major라 glm::mat4가 전치 없이 그대로 실려 온다.
layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) out vec3 fragColor;

void main() {
    // **한 줄이 됐다.** 회전(손계산 2x2)과 종횡비 보정(x /= aspect)이 여기 있었는데
    // 둘 다 행렬이 하는 일이라 CPU로 올라갔다 - 회전은 model로, 보정은 proj로.
    //
    // w로 나누는 것(원근 나눗셈)은 우리가 안 한다. 래스터라이저가 한다.
    // 그래서 **먼 것이 작아지는 일**이 이 한 줄 다음에 일어난다.
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragColor = inColor;
}
