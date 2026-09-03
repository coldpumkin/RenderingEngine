# Dear ImGui

- 업스트림: https://github.com/ocornut/imgui
- 버전: **v1.91.5**
- 고정 커밋: `f401021d5a5d56fe2304056c391e78f81c8d4b8f`

## 왜 들였나

옵션이 여럿이 되면서 키보드 토글로는 못 보게 됐다. 숫자 키 넷까지는 됐지만
material·조명·pass를 각각 껐다 켜려면 화면에 목록이 있어야 한다.

## 왜 이 조합이 맞았나 — 헤더가 둘 다 명시한다

- **커스텀 로더**: `IMGUI_IMPL_VULKAN_NO_PROTOTYPES` + `ImGui_ImplVulkan_LoadFunctions`
  (`imgui_impl_vulkan.h:40,116`). **`IMGUI_IMPL_VULKAN_USE_VOLK`는 쓰면 안 된다** -
  그건 backend가 volk의 *전역* 함수 포인터를 부르게 하는데, 우리는
  `volkLoadInstanceOnly`만 부르고 device 레벨 전역은 비워둔다. 붙여봤더니
  `vkCreateDescriptorPool`이 null이라 앱이 로그 한 줄 없이 죽었다
- **dynamic rendering**: `UseDynamicRendering` + `PipelineRenderingCreateInfo`
  (`imgui_impl_vulkan.h:88,90`). 우리는 `VkRenderPass`를 안 만든다

## 가져온 파일

핵심:

```
imconfig.h  imgui.h  imgui.cpp  imgui_draw.cpp  imgui_tables.cpp
imgui_widgets.cpp  imgui_internal.h
imstb_rectpack.h  imstb_textedit.h  imstb_truetype.h
LICENSE.txt
backends/imgui_impl_glfw.{h,cpp}
backends/imgui_impl_vulkan.{h,cpp}
```

**뺀 것**: `imgui_demo.cpp`(1만 줄, 위젯 카탈로그) · `examples/` · `docs/` ·
`misc/`. demo는 위젯을 구경하는 용도라 필요해지면 그때 한 파일만 더 가져온다.

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

- **자기 descriptor pool을 만든다.** `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`를
  요구하는데 우리 pool은 일부러 그 플래그가 없다(뽑아서 끝까지 쓰므로 반납할 일이
  없다). 그래서 공유하지 않고 ImGui 것을 따로 둔다
- **GUI는 pass 하나다.** post-process pass가 그린 뒤 같은 image에 loadOp=LOAD로
  덧그린다. 1 sample · swapchain format
