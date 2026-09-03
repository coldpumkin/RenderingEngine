# Dear ImGui - 즉시 모드 GUI. 버전과 가져온 파일은 VERSION.md.
#
# 업스트림 CMakeLists를 안 쓴다(방침). 여기가 최소 버전이고, 소스 목록이 곧
# "우리가 벤더링한 것"이다 - 파일이 늘면 VERSION.md와 여기 둘 다 고쳐야 한다.
#
# backend 둘이 창(GLFW)과 그리는 쪽(Vulkan)을 각각 맡는다. 나머지는 플랫폼을 모른다.

set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

add_library(imgui STATIC
    imgui.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp

    backends/imgui_impl_glfw.cpp
    backends/imgui_impl_vulkan.cpp
)

# backends/ 도 PUBLIC: 소비자가 "backends/imgui_impl_vulkan.h"로 부른다.
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# **USE_VOLK가 아니라 NO_PROTOTYPES다.** 전자는 backend가 volk의 *전역* 함수
# 포인터를 부르게 하는데, 우리는 volkLoadInstanceOnly만 부르고 device 레벨 전역은
# 일부러 비워둔다 (Device.cpp - table로 부른다). 그래서 USE_VOLK로 붙이면
# vkCreateDescriptorPool이 null이고 앱이 조용히 죽는다. 실제로 겪었다.
#
# 대신 Gui.cpp가 ImGui_ImplVulkan_LoadFunctions로 로더를 넘긴다.
#
# **PUBLIC이어야 한다.** 이 매크로가 imgui_impl_vulkan.h가 어떤 선언을 보일지 정하는데
# 우리 쪽 .cpp도 그 헤더를 읽는다. PRIVATE이면 라이브러리와 소비자가 서로 다른 선언을
# 보고, 그건 링크될 때까지 조용하다 (GLM_FORCE_DEPTH_ZERO_TO_ONE과 같은 이유).
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

# volk는 헤더를 위해, GLFW는 backend가 창 API를 부르기 위해.
target_link_libraries(imgui PUBLIC volk glfw)

set_target_properties(imgui PROPERTIES FOLDER "ThirdParty")
