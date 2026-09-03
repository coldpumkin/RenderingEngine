# Dear ImGui

- 업스트림: https://github.com/ocornut/imgui
- 버전: **v1.91.5**
- 고정 커밋: `f401021d5a5d56fe2304056c391e78f81c8d4b8f`

## 왜 들였나

옵션이 여럿이 되면서 키보드 토글로는 못 보게 됐다. 숫자 키 넷까지는 됐지만
material·조명·pass를 각각 껐다 켜려면 화면에 목록이 있어야 한다.

## Vulkan backend는 **안 가져온다**

`imgui_impl_vulkan`을 한 번 썼다가 뺐다. 그게 들고 오는 것이 전부 이미 있는 것이었다:

| backend가 만드는 것 | 우리 것 |
|---|---|
| 자기 `VkPipeline` | `Pipeline` + `gui.vert/frag` |
| 자기 descriptor pool (`FREE_DESCRIPTOR_SET_BIT` 요구) | `Descriptors`의 pool |
| 자기 함수 로더 | `VolkDeviceTable` |
| 자기 정점 버퍼 | `Buffer` |

그리고 요구하는 값 중에 **우리가 답할 수 없는 것**이 있었다 - `MinImageCount`와
`ImageCount`다. swapchain은 첫 프레임에야 생기므로 초기화 시점에 줄 수 있는 건
추측뿐이고, 실제로 주석에 *"not a promise about our swapchain"*이라고 적어야 했다.

**함정도 하나 있었다**: `IMGUI_IMPL_VULKAN_USE_VOLK`를 붙이면 backend가 volk의 *전역*
함수 포인터를 부른다. 우리는 `volkLoadInstanceOnly`만 부르고 device 레벨 전역은
비워두므로 `vkCreateDescriptorPool`이 null이고, **앱이 로그 한 줄 없이 죽었다.**

그래서 남은 것은 ImGui가 실제로 잘하는 것 하나다 — **위젯 호출을 삼각형 목록으로
바꾸는 것**. 그리기는 `Gui.cpp`가 한다.

## 가져온 파일

핵심:

```
imconfig.h  imgui.h  imgui.cpp  imgui_draw.cpp  imgui_tables.cpp
imgui_widgets.cpp  imgui_internal.h
imstb_rectpack.h  imstb_textedit.h  imstb_truetype.h
LICENSE.txt
backends/imgui_impl_glfw.{h,cpp}
```

**뺀 것**: `backends/imgui_impl_vulkan.{h,cpp}`(위 참조) · `imgui_demo.cpp`(1만 줄,
위젯 카탈로그) · `examples/` · `docs/` · `misc/`.

## CMake

업스트림 CMakeLists를 안 쓴다(방침). `CMakeLists.txt`가 최소 버전이고
`IMGUI_IMPL_VULKAN_USE_VOLK`를 타깃에 붙여서 소비자가 깜빡할 수 없게 한다 —
GLM의 `GLM_FORCE_DEPTH_ZERO_TO_ONE`과 같은 이유다.

`/W4` 상속은 `set_directory_properties(PROPERTIES COMPILE_OPTIONS "")`로 끊는다.

## 갱신 방법

```
git clone --depth 1 --branch <태그> https://github.com/ocornut/imgui.git
```

위 "가져온 파일" 목록을 덮어쓰고, 이 파일의 버전·커밋을 고친다.
backend 두 개는 상단 주석에 변경 이력이 있으니 갱신 시 읽는다.

## 우리 쪽 계약

- **GUI는 pass 하나다.** post-process pass가 그린 뒤 같은 image에 loadOp=LOAD로
  덧그린다. 1 sample · swapchain format
- **정점이 매 프레임 바뀐다.** 이 저장소에서 처음이다. mesh는 staging으로 한 번
  올려 `DEVICE_LOCAL`에 두지만, 이건 `HOST_VISIBLE` + mapped에 frames-in-flight마다
  한 쌍씩 두고 기록할 때 쓴다
- **font atlas는 `Texture` 하나다.** `RGBA8_UNORM` - 색이 아니라 커버리지라
  SRGB로 읽으면 글자가 얇아진다 (normal map과 같은 구분)
