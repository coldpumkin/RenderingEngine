#version 450

// 전체화면 삼각형. **정점 버퍼가 없다** - gl_VertexIndex로 세 점을 만들어낸다.
//
// 사각형(정점 4개) 대신 화면보다 큰 삼각형 하나를 쓴다. 시저가 잘라주니 결과는 같고,
// 사각형은 두 삼각형이 만나는 대각선의 픽셀이 두 번 계산된다.
//
//   index 0 -> uv(0,0) -> pos(-1,-1)
//   index 1 -> uv(2,0) -> pos( 3,-1)
//   index 2 -> uv(0,2) -> pos(-1, 3)
layout(location = 0) out vec2 uv;

void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
