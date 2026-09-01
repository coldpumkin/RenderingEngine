#version 450

// 정점에서 넘어온 색을 그대로 쓴다. 래스터라이저가 세 정점 사이를 보간해준다.
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUV;

// 아래 둘이 C++의 sceneLayout과 짝이고, 어느 컴파일러도 양쪽을 같이 안 본다.
// **이쪽이 원본이다** - C++은 여기 적힌 개수와 자리를 받아적는다.
//
// fullscreen.frag는 sampler2D가 하나라 presentLayout으로 갈렸다.
layout(set = 0, binding = 0) uniform sampler2D tex;

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
    // 곱한다. texture가 흰 칸이면 정점 색 그대로, 어두운 칸이면 어두워진다 -
    // 물체마다 다른 색을 유지한 채 무늬만 얹힌다.
    const vec3 base = texture(tex, fragUV).rgb;
    outColor = vec4(fragColor * base, pc.alpha);
}
