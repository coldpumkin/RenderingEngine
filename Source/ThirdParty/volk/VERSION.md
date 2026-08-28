# volk

- 업스트림: https://github.com/zeux/volk
- 고정 커밋: `2e19a77ce9b5bd8df0106f66b91032c0e5c776a1`
- `VOLK_HEADER_VERSION`: 360
- 가져온 파일: `volk.h`, `volk.c`, `LICENSE.md` (그 외 업스트림 파일은 안 씀)
- 라이선스: MIT (`LICENSE.md`)

## FetchContent 대신 벤더링한 이유

- volk는 `.c` 하나 + `.h` 하나다. 받아오는 비용보다 저장소에 두는 비용이 더 싸다.
- **`volk.c`를 직접 읽는 게 이 프로젝트의 학습 목표에 포함된다.** 함수 포인터를
  어떻게 로드하는지 보려고 매번 빌드 캐시(`out/_deps/`) 안을 뒤지는 건 말이 안 된다.
- 네트워크 없이도 configure가 되고, VS로 폴더를 열 때 다운로드 대기가 없다.

대가: 업스트림 업데이트가 수동이다. 갱신할 땐 위 세 파일만 덮어쓰고 이 문서의
커밋 해시와 `VOLK_HEADER_VERSION`을 같이 고친다.

## 요구사항

Vulkan SDK가 설치돼 있어야 한다 (헤더만 쓴다). `find_package(Vulkan)`이
`VULKAN_SDK` 환경변수로 찾는다. 확인된 환경: `C:\VulkanSDK\1.4.350.0`
