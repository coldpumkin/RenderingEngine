# stb_image

PNG·JPEG를 픽셀 배열로 푼다. 단일 헤더 C 라이브러리이고 의존성이 없다.

## 출처

```
업스트림: https://github.com/nothings/stb
버전:     stb_image v2.30
커밋:     013ac3beddff3dbffafd5177e7972067cd2b5083  (2024-05-31)
라이선스: MIT 또는 public domain (둘 중 고를 수 있다)
```

가져온 파일은 하나다.

```
stb_image.h    283,010 bytes / 7,103 줄
               sha256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
```

`stb_image.c`는 우리가 쓴 것이다 - `STB_IMAGE_IMPLEMENTATION`을 정의하고 헤더를
include하는 TU다. 업스트림에는 그런 파일이 없다.

**태그가 아니라 커밋을 고정한 이유**: stb는 릴리스 태그를 안 단다. 저장소가 여러
라이브러리를 함께 담고 있어서 stb_image.h를 마지막으로 건드린 커밋이 기준이 된다.

## 왜 벤더링인가

cgltf와 같다. 파일이 하나라 복사가 제일 싸고, 네트워크 없이 configure되고, 소스를
직접 읽는 것이 학습 목표에 든다.

## 무엇을 끄고 쓰나

`stb_image.c`가 디코더를 둘만 켠다.

```c
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
```

Sponza가 참조하는 69장이 jpg 65 · png 4이고, 나머지 포맷(BMP·TGA·PSD·GIF·HDR·PIC·PNM)은
쓰지 않는다. 켤 일이 생기면 여기 한 줄이고, 잊었을 때의 실패는 조용하지 않다 -
`stbi_load`가 null과 `"unknown image type"`을 돌려준다.

**stdio는 켜 둔다.** 바이트를 우리가 읽으면 `Shader.cpp`의 `ReadSpirv` 옆에 두 번째
파일 리더가 생기는데, 그것과 공유할 수 없다(`uint32_t`를 돌려주고 크기가 4의 배수여야
한다). 서로 다른 리더 둘보다 stb가 자기 파일을 여는 편이 낫다.

## 들이기 전에 확인한 것

`Spikes/ImageProbe/`에서 Sponza가 참조하는 69장을 전부 풀어봤다. 검증 대상은
"stb_image가 도는가"가 아니라 **"우리 업로드 경로가 받는 모양으로 주는가"**였다 -
`CreateTextureFromPixels`가 RGBA8 · 4채널 · 빈틈없는 배열을 받는다.

```
ok 69 / failed 0
RGBA8 로 풀면        272.0 MB   (파일은 41 MB)
mip 체인까지          362.7 MB
크기                  4x4 ~ 1024x1024,  2048 초과 0장
원본 채널             rgb 66 · rgba 3   (항상 4채널로 요청한다)
```

**272 MB가 이 라이브러리를 쓸 때 알아야 할 숫자다.** jpg 41 MB가 GPU에서는 7배가 된다.
줄이려면 BC7 같은 블록 압축이 필요하고, 그건 라이브러리가 하나 더 붙는 결정이라
재보기 전에는 근거가 없다.

## 갱신 방법

1. 위 저장소에서 `stb_image.h`를 받아 이 폴더에 덮어쓴다
2. 이 파일의 커밋·sha256·버전을 고친다
3. `Spikes/ImageProbe/`의 것도 같이 바꿔 단독으로 한 번 돌린다
4. `check.ps1`로 Engine 전체를 확인한다

업스트림 CMakeLists는 없다. 여기 있는 것이 우리가 직접 쓴 최소 버전이다.
