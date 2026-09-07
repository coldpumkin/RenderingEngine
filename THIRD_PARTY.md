# 서드파티

`Source/ThirdParty/` 아래에 벤더링했다. 각 디렉터리의 `VERSION.md`에 업스트림 URL과 가져온
파일, 갱신 방법이 적혀 있다.

벤더링한 이유는 셋이다 — 네트워크 없이 configure가 되고, 버전이 커밋에 박히고, **그 소스를
직접 읽는 것이 이 프로젝트의 목적에 포함되기 때문**이다. 업스트림의 CMakeLists를 그대로 쓰지
않고 최소한의 것을 직접 썼고, 서드파티 디렉터리에서는 `/W4` 상속을 끊는다.

| | 무엇 | 라이선스 | 이 저장소에 들어 있는 것 |
|---|---|---|---|
| [volk](https://github.com/zeux/volk) | Vulkan 함수 포인터 로더 | MIT | 소스 + `LICENSE.md` |
| [Dear ImGui](https://github.com/ocornut/imgui) | 패널 | MIT | 소스 + `LICENSE.txt` |
| [GLM](https://github.com/g-truc/glm) | 수학 | MIT (Happy Bunny) | 헤더 + `LICENSE.txt` |
| [cgltf](https://github.com/jkuhlmann/cgltf) | glTF 파서 | MIT (파일 끝의 고지) | `cgltf.h` |
| [stb_image](https://github.com/nothings/stb) | 이미지 디코더 | public domain / MIT (파일 끝의 고지) | `stb_image.h` |
| [SPIRV-Reflect](https://github.com/KhronosGroup/SPIRV-Reflect) | SPIR-V 리플렉션 | Apache-2.0 (파일 머리의 고지) | 소스 |
| [GLFW](https://github.com/glfw/glfw) | 창과 입력 | Zlib | **소스 없음.** CMake `FetchContent`로 빌드 때 받는다 |
| [VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | GPU 메모리 할당 | MIT | **소스 없음.** 헤더는 Vulkan SDK의 것을 쓰고, 구현을 펼치는 `vma.cpp` 한 파일만 있다 |

GLFW만 벤더링하지 않은 이유는 파일 수다. volk는 두 파일이고 그 로딩 방식이 읽을 거리지만,
GLFW는 수십 파일이고 내부가 이 프로젝트의 학습 대상이 아니다. 대가는 최초 configure에
네트워크가 필요하다는 것이고, 그 뒤로는 캐시된다.

VMA는 헤더 하나에 선언과 구현이 다 들어 있어서(약 2만 줄) `VMA_IMPLEMENTATION`을 정의하는
번역 단위를 하나로 고정했다. `VMA_STATIC_VULKAN_FUNCTIONS`와 `VMA_DYNAMIC_VULKAN_FUNCTIONS`를
둘 다 0으로 두고 `VmaVulkanFunctions`를 직접 채워 넘긴다 — volk와 같이 쓰기 때문이고,
그래야 VMA가 우리 device table과 같은 함수 포인터를 쓴다.
