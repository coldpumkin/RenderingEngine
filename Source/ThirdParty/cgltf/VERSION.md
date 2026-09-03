# cgltf

glTF 2.0 파일을 읽는다. 단일 헤더 C 라이브러리이고, 의존성이 없다.

## 출처

```
업스트림: https://github.com/jkuhlmann/cgltf
버전:     v1.15 (2025-02-09)
커밋:     360db1a95480fe102ae9c69b27c5d101167ff5ba
라이선스: MIT (Copyright 2018-2021 Johannes Kuhlmann)
```

가져온 파일은 하나다.

```
cgltf.h    202,865 bytes / 6,186 줄
           sha256 e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec
```

`cgltf.c`는 우리가 쓴 것이다 - `CGLTF_IMPLEMENTATION`을 정의하고 헤더를 include하는
네 줄짜리 TU다. 업스트림에는 그런 파일이 없다.

## 왜 벤더링인가

방침 그대로다. 파일이 **하나**라 복사가 제일 싸고, 네트워크 없이 configure되고,
소스를 직접 읽는 것이 학습 목표에 든다. GLFW가 FetchContent인 것은 빌드 시스템이
커서였는데 여기엔 해당이 없다.

## 왜 cgltf인가

| | 언어 | 의존성 | 크기 |
|---|---|---|---|
| **cgltf** | C, 단일 헤더 | **없음** | 6천 줄 |
| tinygltf | C++ | nlohmann/json + stb_image | 큼 |
| fastgltf | C++17 | 있음 | 큼 |

의존성이 없다는 것이 결정적이었다. tinygltf를 쓰면 JSON 파서와 이미지 디코더가
따라 들어오는데, 둘 다 지금 필요 없고 각각이 또 하나의 벤더링 결정이 된다.

## 들이기 전에 확인한 것

`Spikes/GltfProbe/`에서 단독으로 돌려 Sponza를 읽혔다. 같은 값을 `.gltf`(JSON)를
파이썬으로 직접 파싱해서도 뽑아 **두 결과가 전부 일치하는 것**을 보고 들였다 -
라이브러리가 파일을 우리 예상대로 해석하는지가 검증 대상이었다.

```
scenes 1  nodes 1  meshes 1  materials 25  images 69  textures 69
primitives 103   vertices 192,496   indices 786,801   triangles 262,267
largest primitive 23,038 vertices  -> uint16 인덱스로 충분
attributes: POSITION/NORMAL/TEXCOORD_0 전부, TANGENT 102/103
node transform: scale 0.008 하나, 자식 없음 -> 재귀 불필요
```

## 쓰는 법

```c
cgltf_options options = {0};
cgltf_data* data = NULL;
cgltf_parse_file(&options, path, &data);   // JSON만 읽는다
cgltf_load_buffers(&options, data, path);  // 옆의 .bin을 연다. 이걸 빼면 데이터가 없다
cgltf_validate(data);
...
cgltf_free(data);
```

정점을 꺼낼 때는 `cgltf_accessor_read_float`를 쓴다. accessor의 componentType이
무엇이든(normalized byte, short, float) float로 풀어주고 stride도 처리한다 -
이 라이브러리를 쓰는 이유의 절반이 이 함수다.

## 갱신 방법

1. 위 저장소에서 원하는 태그의 `cgltf.h`를 받아 이 폴더에 덮어쓴다
2. 이 파일의 버전·커밋·sha256을 고친다
3. `Spikes/GltfProbe/`의 것도 같이 바꿔 단독으로 한 번 돌린다
4. `check.ps1`로 Engine 전체를 확인한다

업스트림 CMakeLists는 쓰지 않는다. 여기 있는 최소 버전이 우리가 직접 쓴 것이다.
