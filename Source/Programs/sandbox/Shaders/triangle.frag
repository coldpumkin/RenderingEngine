#version 450

// 정점에서 넘어온 색을 그대로 쓴다. 래스터라이저가 세 정점 사이를 보간해준다.
layout(location = 0) in vec3 fragColor;

layout(location = 0) out vec4 outColor;

// vertex shader와 같은 블록이다. 여기서 alpha만 읽는데도 mvp까지 적어야 한다 -
// 블록 하나를 두 stage가 나눠 보는 것이라 선언이 어긋나면 컴파일이 막는다.
// C++ 쪽 pushRange.stageFlags에 FRAGMENT가 없으면 validation layer가 잡는다.
layout(push_constant) uniform Push {
    mat4 mvp;
    float alpha;
} pc;

void main() {
    // alpha는 blend가 켜진 pipeline에서만 의미가 있다. 불투명 pipeline은
    // blendEnable이 꺼져 있어 이 값이 통째로 무시된다.
    outColor = vec4(fragColor, pc.alpha);
}
