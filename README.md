# RenderingEngine

Vulkan 1.3 기반 실시간 렌더링 엔진. 한 프레임이 어떻게 조립되는지를 코드에서 위에서
아래로 읽을 수 있는 상태를 목표로 썼다.

![Sponza, forward 경로](docs/images/sponza-forward.jpg)

*Sponza · forward · MSAA 4x · directional + spot + point 광원, 광원마다 그림자 맵,
절차적 하늘에서 bake한 IBL*

---

## 무엇을 하는가

한 프레임이 두 종류이고, 패널의 체크박스가 고른다.

```
forward    shadow  sky  scene                 post  gui
deferred   shadow  sky  geometry  lighting    post  gui
```

두 경로는 **같은 이미지를 채운다.** forward의 scene pass가 resolve로 내보내는 그 이미지에
deferred의 lighting pass가 그린다. 그래서 앞뒤(shadow · sky · post · gui)가 전부 같고,
`RecordFrame`의 분기 네 줄이 두 경로의 차이 전부다. 두 경로 다 시작할 때 만들어져 각자
descriptor set을 들고 있으므로, 스위치는 아무것도 다시 만들지 않는다.

| | forward | deferred |
|---|---|---|
| 샘플 수 | MSAA 4x | 1 sample |
| 조명 | 표면마다 한 번 | 화면 픽셀마다 한 번 (fullscreen 삼각형 하나) |
| 중간 저장 | 없음 | G-buffer 4장 (albedo · normal · metallic-roughness · depth) |

MSAA를 deferred에 맞추지 않았다. 샘플마다 조명을 따로 해야 하고, **평균 낸 normal은 어느
표면의 것도 아니기** 때문이다. 비교하는 대상은 구조이고, 가장자리는 다르게 나오는 것이 맞다.

---

## 구현된 것

- **dynamic rendering** — `VkRenderPass`도 `VkFramebuffer`도 만들지 않는다
- **그림자** — 광원마다 2048² depth map. 배열 텍스처의 층이고, **광원 i가 matrix i와 층 i를
  쓴다**. 번역하는 것이 없다
- **광원 세 종류** — directional · spot · point. `LightKind`가 소유를 정한다: directional은
  위치가 없고(무한히 먼 것의 이상화라 세계의 성질이다), 나머지는 있다. shader에 가는
  `LightEntry`에는 **kind 필드가 없다** — point light는 원뿔이 끝까지 열린 spot이라 같은 식이
  둘을 다 처리하고, `direction.w`의 0/1은 플래그가 아니라 동차좌표의 뜻 그대로다
- **IBL** — 시작할 때 넷을 bake한다. 하늘 큐브(256², 절차적) · irradiance 큐브(32²,
  반구 코사인 적분) · prefiltered 큐브(128² 5레벨, GGX) · BRDF LUT(256² RG16F).
  split-sum 근사이고, **환경 텍스처 파일이 없다** — 하늘이 방향의 함수라서다
- **직접광은 Blinn-Phong이다.** GGX도 Fresnel도 에너지 보존도 없다. roughness가 에셋에서
  와서 highlight를 좌우한다는 것까지가 지금 하는 일이고, 그 이상을 하는 척하지 않는다
- **glTF 로딩** — Sponza 103 primitives / 192,496 정점 / 786,801 index / 25 materials.
  TANGENT가 없는 primitive는 생성한다(있는 102개와 대조해 평균 dot 0.9953으로 검증)
- **MSAA 4x · frames-in-flight 2 · swapchain image 3 · VMA · 리사이즈/최소화 대응**

---

## 설계에서 실제로 판단한 것

### 값마다 "언제 확정되는가"를 묻는다

Vulkan은 값을 언제 정할지 강제로 묻는다. 그 질문에 대한 답이 곧 그 값이 사는 자리다.

| 언제 확정 | 어디로 | 예 |
|---|---|---|
| pipeline 생성 | `VkGraphicsPipelineCreateInfo` | attachment format · sample 수 · vertex layout · blend · polygonMode |
| 자원 생성 | descriptor set | 카메라 · 광원 · 그림자 matrix · 환경 큐브 · material |
| draw마다 | push constant | model matrix + normal matrix + alpha (116 / 128 B) |
| 커맨드마다 | `vkCmdSet*` | viewport · scissor · cull · depth test/write/compare |

**기준은 빈도가 아니라 "드라이버가 다른 것을 컴파일하는가"다.** viewport와 cullMode는
레지스터라 dynamic이 거의 공짜지만, blend와 sample 수와 attachment format은 컴파일 결과를
바꾸므로 pipeline에 들어간다. 그래서 wireframe은 pipeline이 하나 더 있고(`polygonMode`는
컴파일된다), depth test는 체크박스가 커맨드 하나로 끝난다.

### 선언하게 하고, 그 선언이 가능한지를 독립된 사실로 검증한다

발견하면 검증할 것이 안 남는다. 그래서 상위가 의도를 **선언**하고, 그것과 무관하게 존재하는
사실이 그 선언을 **검증**한다. 지금 일곱 자리가 그 모양이다.

| 선언하는 쪽 | 검증하는 쪽 |
|---|---|
| `MaterialSet()` — set 1의 구성 | shader reflection (타입 + **이름**까지) |
| `SharedBlocks()` — uniform block 다섯의 멤버 offset·size | shader reflection |
| `VertexInput()` — vertex layout | shader reflection |
| `Attachment.role` — 이 pass가 무엇으로 쓰는가 | 그 이미지의 format과 usage |
| `Attachment.resolve` — 무엇을 남기는가 | `BeginPass`가 실제 view와 대조 |
| `AttachmentFormats` — pipeline이 무엇에 대해 컴파일됐는가 | pass가 선언한 것과 대조 |
| `ImageRequirement` — shader가 무엇을 읽는가 | `UpdateSet`이 실제 view와 대조 |

**offset을 손으로 적은 숫자가 하나도 없다.** `SharedBlocks()`는 `offsetof`와 `sizeof`로
쓰여 있어서, C++ 구조체의 필드를 옮기면 요구도 같이 움직이고 따라오지 않은 shader는 시작할 때
거절된다. 실제로 이 검증이 두 번 발화했다 — 광원 uniform이 자랄 때 이름으로, 줄어들 때
없어진 멤버로.

### 층마다 자기가 가진 사실로만 검증한다

```
Pass 선언       resource · role · load/store · resolve      의도
      ^
      | 검증     사실이 의도를 검증한다. 반대가 아니다
      |
Resource 사실   format · usage · samples · extent · 정체
      |
      | 사영     정체가 여기서 버려진다
      v
Pipeline 계약   color/depth/stencil format · samples
```

pipeline은 자원을 모른다 — format과 sample 수만 내려가고 *어느 이미지인가*는 안 내려간다.
그래서 **정체는 사영으로 확인할 수 없고 양쪽에서 따로 물어야 한다.** 프레임마다 달라질 수
있는 것만 프레임에서 묻는다는 규칙도 여기서 나온다: `ValidatePassDesc(const RenderPassDesc&)`는
인자에 프레임이 없어서 프레임 의존 검사를 **쓸 수 없다.**

### 검증할 수 없는 것을 검증하는 척하지 않는다

`.spv`가 기록하는 것은 shader의 시야다 — 숫자 종류, 컴포넌트 수, location, set, binding, 이름.
**`VkFormat`은 자원의 것이라 안 적힌다.** SRGB인지 UNORM인지 shader는 영원히 모르므로,
normal map을 SRGB로 읽는 실수는 어떤 검사로도 잡히지 않는다. 그런 자리는 코드에 `Contract:`로
남아 있고, 지금 네 개다(SRGB/UNORM 둘, vertex packing 둘).

---

## 도구

![패널](docs/images/panel.jpg)

*왼쪽 패널은 스위치를 **값이 GPU까지 가는 경로로 묶어서** 보여준다 — passes(분기) ·
lighting(uniform) · raster(커맨드) · pipeline(컴파일). 오른쪽은 descriptor pool·shader
interface·frame 통계를 읽는 창이다*

- **그림 회귀 검사** — `LAMBDA_CAPTURE=<경로>.bmp`면 둘째 프레임의 swapchain 이미지를 파일로
  쓰고 종료한다. 시간이 고정되므로 같은 바이너리를 두 번 돌리면 sha256이 같고,
  `Tools/capture.ps1`이 두 경로를 각각 돌려 기준값과 대본다. 배리어나 draw 순서를 건드리면
  숫자가 움직인다
- **검증 레이어 · 동기화 검증**을 기준선 0건으로 유지한다. 구조를 바꾼 커밋은 리사이즈 4회와
  최소화/복원까지 돌려서 확인했다
- **draw 통계** — 103 draws / 25 material binds / 2 cull changes. material bind는 실제 material
  수와 같고 cull change는 2라, 지금 sort key `(cullMode, material)`는 둘 다 바닥이다

---

## 빌드

필요한 것: **Vulkan SDK**(헤더와 `glslc`), CMake 3.24+, C++20 컴파일러.
GLFW만 FetchContent로 받고 나머지 third-party는 `Source/ThirdParty/`에 벤더링되어 있다.

```
cmake --preset default
cmake --build --preset default
```

Windows/MSVC에서는 `vcvars64.bat` 환경 안에서 돌린다. 콘솔 codepage는 UTF-8(`chcp 65001`)이어야
한다 — CP949에서는 `cl /showIncludes`의 출력이 ninja가 기대하는 접두어와 어긋나 **헤더 의존성이
한 줄도 기록되지 않는다.** 빌드는 성공하는데 헤더를 고쳐도 다시 컴파일되지 않는 상태가 되고,
증상은 링크 후 런타임 크래시로 나타난다.

### 에셋

저장소에 모델이 없다. Khronos의
[glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)에서
`Models/Sponza/glTF`를 받아 `Assets/Sponza/`에 둔다. 에셋이 없으면 로그를 남기고 종료한다 —
대신 그릴 것을 만들지 않는다.

조작: `WASD`·`QE` 이동, 화살표 회전.

---

## 구조

```
Source/Programs/sandbox/
  Vulkan/            API 자원. 개념당 .h/.cpp 한 쌍
                     Core · Instance · Device · Window · Swapchain · Commands · Frame
                     Image · Attachments · Texture · Descriptors · Barrier
                     Pipeline · Shader · Buffer · Mesh · VertexLayout
  Passes.h/.cpp      pass들이 공유하는 것과 프레임의 순서
  ShadowPass         광원의 시점에서 depth만
  ScenePass          forward. MSAA 4x
  GeometryPass       deferred 전반. G-buffer 넷
  LightingPass       deferred 후반. fullscreen 삼각형 하나
  PostProcessPass    가운데의 결과를 swapchain으로
  Sky.h/.cpp         환경 bake 넷과 하늘을 그리는 pass
  Gui.h/.cpp         패널 (ImGui)
  Pipelines.h/.cpp   ShaderProgram 열하나와 Pipeline 열넷
  Renderer.h         device가 있어야 존재하는 것 전부
  main.cpp           창 · 입력 · 프레임 루프
```

**화살표가 한쪽이다.** `Passes.h`는 `Vulkan/`의 타입을 이름으로 쓰지만, `Vulkan/` 아래 어느
헤더도 pass를 모른다. `main`이 드는 것은 device를 **만드는** 것뿐이고(windowSystem · instance ·
device · window · commands), device가 있어야 존재하는 것은 전부 `Renderer`에 있다.

pass가 자기 자원을 만들지 않는다. `main`이 만들어 양쪽에 같은 배열을 넘긴다 — 그래서 그림자
맵을 그리는 pass와 읽는 pass가 **같은 이름으로 같은 이미지를 가리킨다.**

---

## 아직 없는 것

- point light의 큐브 그림자 (2D map은 한 방향이고 point light는 모든 방향이다)
- mesh는 하나. sort key에 축을 더할 근거가 아직 없다
- shader 런타임 컴파일, 삭제 큐(deletion queue)
- compute/transfer 큐는 만들어만 두고 제출은 하지 않는다
- pass 사이의 의존을 데이터로 적는 것(RDG). 지금은 `RecordFrame`의 줄 순서가 그 답이고,
  선언과 순서 검사(`CheckPassOrder`)까지는 들어와 있다

---

## 서드파티

volk · Dear ImGui · GLM · cgltf · stb_image · SPIRV-Reflect를 `Source/ThirdParty/`에
벤더링했다. GLFW는 빌드 때 `FetchContent`로 받고, VMA는 헤더를 Vulkan SDK에서 쓴다.
업스트림·라이선스·왜 그렇게 나뉘는지는 [THIRD_PARTY.md](THIRD_PARTY.md)에 있다.
