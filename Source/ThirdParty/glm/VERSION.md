# GLM (OpenGL Mathematics)

- 업스트림: https://github.com/g-truc/glm
- 태그: `1.0.3`
- 고정 커밋: `8d1fd52e5ab5590e2c81768ace50c72bae28f2ed`
- 라이선스: The Happy Bunny License 또는 MIT (`LICENSE.txt` — 업스트림 `copying.txt`)

## 가져온 파일

업스트림의 `glm/` 트리에서 **뺀 것이 있다**. 헤더 300개 / 약 2.4MB가 들어왔다.
`.hpp`/`.inl`/`.h` 말고는 아무것도 없다.

| | |
|---|---|
| 가져옴 | `glm/` (core · `detail/` · `ext/` · `gtc/` · `simd/`), `copying.txt` → `LICENSE.txt` |
| 뺌 | `glm/gtx/` (129개) — 업스트림이 experimental로 표시한 확장 |
| 뺌 | `glm/ext.hpp` — `gtx/`를 include하는 유일한 파일. 업스트림도 이걸 쓰지 말라고 적어놨다 |
| 뺌 | `glm/detail/glm.cpp` — GLM을 정적 라이브러리로 빌드할 때 쓰는 것. 우리는 헤더 온리다 |
| 뺌 | `glm/CMakeLists.txt`, `glm/glm.cppm` — 우리 CMakeLists를 쓰고, C++20 모듈은 안 쓴다 |
| 뺌 | `test/`, `doc/`, `util/`, `cmake/`, 최상위 `CMakeLists.txt` |

**`#include <glm/ext.hpp>`는 안 된다.** 없는 파일이라 컴파일 에러가 난다.
필요한 확장을 하나씩 include한다 (`glm/ext/matrix_clip_space.hpp` 같은 식으로).
`gtx/`의 무언가가 실제로 필요해지면 그때 그 파일만 가져오고 이 표를 고친다.

## 갱신 방법

```
git clone --depth 1 --branch <태그> https://github.com/g-truc/glm.git
```

`glm/` 트리를 통째로 덮어쓴 뒤 위 "뺌" 항목을 다시 지운다.
그리고 **`Spikes/GlmCheck`를 돌려서** 아래 넷이 그대로인지 확인하고,
이 문서의 태그·커밋을 고친다.

## Spike가 확인한 것 (`Spikes/GlmCheck`)

벤더링한 바로 그 파일들을 `/W4`로 빌드해서 얻은 값이다.

| 확인 | 결과 |
|---|---|
| `sizeof(glm::mat4)` | 64. push constant 128B 한도의 절반이라 `time`/`aspect`와 같이 실린다 |
| 메모리 레이아웃 | column-major. `translate(7,8,9)`의 `value_ptr[12..14]`가 `7 8 9`. **GLSL `mat4`와 같아서 그대로 memcpy된다** |
| `GLM_FORCE_DEPTH_ZERO_TO_ONE` | 켜면 `glm::perspective == perspectiveRH_ZO`. near→`z=0.000`, far→`z=1.000`. 안 켜면 `-1.000`/`1.000`(OpenGL) |
| y 방향 | `proj[1][1] = +1.732` (**양수 = GLM은 y를 안 뒤집는다**) |
| `/W4` 경고 | 0개. 그래서 include를 `SYSTEM`으로 안 감쌌다 |

## y-flip — 두 번 하면 안 된다

Vulkan 기본 NDC는 y가 아래로 향한다. 우리는 이미 **viewport height를 음수**로 줘서
뒤집어놨다 (`sandbox/main.cpp`의 `RecordScenePass`). GLM proj는 y를 그대로 두므로
**우리 뒤집기 하나로 맞는다.**

흔히 보이는 `proj[1][1] *= -1`은 viewport가 양수 height일 때 쓰는 것이다.
우리 코드에서 그걸 같이 하면 이중 반전이 되어 화면이 상하로 뒤집힌다.

**곁딸린 것**: negative viewport height는 y뿐 아니라 삼각형의 winding 판정도 뒤집는다.
지금은 `cullMode = VK_CULL_MODE_NONE`(`Vulkan/Pipeline.cpp`)이라 안 보이지만,
back-face culling을 켜는 순간 CCW로 적은 앞면이 사라진다.

## 요구사항

없다. 헤더 온리이고 SDK에도 의존하지 않는다.
