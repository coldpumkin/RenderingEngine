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
// **z가 있다.** 전에는 여기서 0.0을 박았다 - 모든 것이 같은 깊이라 뎁스 테스트가
// 의미가 없었다. 이제 정점이 자기 깊이를 들고 온다.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

// **셰이더가 처음으로 정점 말고 다른 것을 받는다.**
// C++의 PushConstants와 필드 순서·타입이 정확히 같아야 한다 (Pipeline.h).
layout(push_constant) uniform Push {
    float time;
    float aspect;
} pc;

layout(location = 0) out vec3 fragColor;

void main() {
    // 원점 기준 회전. 두 삼각형이 **같은 각도로 같이** 돌기 때문에 서로의 앞뒤 관계는
    // 변하지 않는다 - 회전 중에도 겹친 곳은 계속 가까운 쪽 색이어야 한다.
    float c = cos(pc.time);
    float s = sin(pc.time);
    vec2 rotated = vec2(inPosition.x * c - inPosition.y * s,
                        inPosition.x * s + inPosition.y * c);

    // **종횡비 보정.** NDC는 항상 [-1,1]인데 화면은 정사각형이 아니다. 보정이 없으면
    // 창을 옆으로 늘릴 때 도형도 같이 늘어난다. x를 aspect로 나눠 가로를 좁힌다.
    //
    // **회전 뒤에 한다.** 순서를 바꾸면 회전이 찌그러진 좌표계에서 일어나서
    // 도형이 돌면서 모양이 변한다.
    rotated.x /= pc.aspect;

    // z는 그대로 넘긴다. 회전은 xy 평면 안에서만 일어나므로 깊이가 안 바뀐다.
    gl_Position = vec4(rotated, inPosition.z, 1.0);
    fragColor = inColor;
}
