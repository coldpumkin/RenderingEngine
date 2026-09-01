# SPIRV-Reflect

SPIR-V 바이트코드에서 shader의 인터페이스(vertex input · push constant ·
descriptor binding)를 읽어낸다.

## 출처

Vulkan SDK 1.4.350.0에 딸려 온 것을 그대로 복사했다.

```
C:\VulkanSDK\1.4.350.0\Source\SPIRV-Reflect\spirv_reflect.h
C:\VulkanSDK\1.4.350.0\Source\SPIRV-Reflect\spirv_reflect.c
C:\VulkanSDK\1.4.350.0\Source\SPIRV-Reflect\include\spirv\unified1\spirv.h
```

업스트림: https://github.com/KhronosGroup/SPIRV-Reflect
라이선스: Apache-2.0 (Copyright 2017-2022 Google Inc.)

## 왜 벤더링인가

VMA는 SDK의 `Include/` 아래에 있어 `Vulkan::Headers`만 링크하면 됐다. 이것은
`Source/` 아래에 소스로만 있어서 헤더 경로에 안 잡히고, `.c`를 우리가 빌드해야
한다. 그래서 복사가 유일한 선택이었다.

## 갱신 방법

SDK를 올린 뒤 위 세 파일을 다시 복사한다. `include/` 아래 구조를 유지해야 한다 -
`spirv_reflect.c`가 `"spirv/unified1/spirv.h"`로 찾는다.

## 왜 필요한가

이 정보들이 지금 C++과 GLSL 양쪽에 손으로 적혀 있고, 어느 컴파일러도 둘을 같이
안 본다. 방어는 `Contract:` 주석이 전부였다:

| C++ | GLSL |
|---|---|
| `VertexInput()`의 attribute 셋 | `triangle.vert`의 `layout(location=N) in` |
| `PushConstants` + `pushRange` | `layout(push_constant)` 블록 |
| `kSceneBindingCount` | `triangle.frag`의 `sampler2D` 개수 |
| `kPresentBindingCount` | `fullscreen.frag`의 `sampler2D` 개수 |

**SPIR-V 안에 이미 다 있다.** 2026-09-01에 pipeline 생성에 틀린 layout을 줬더니
검증 레이어가 SPIR-V를 읽어 `"detail"`이라는 GLSL 변수명으로 잡았다.

## 도입 방식

**`Spikes/`를 건너뛰고 바로 들였다** (2026-09-02, 사용자 결정). 방침의 예외이고,
문제가 나오면 그때 분리해서 좁힌다.
