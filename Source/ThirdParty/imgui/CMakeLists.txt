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
)

# backends/ 도 PUBLIC: 소비자가 "backends/imgui_impl_vulkan.h"로 부른다.
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# **Vulkan backend는 안 쓴다.** ImGui가 imgui_impl_vulkan을 주지만 우리가 직접 그린다
# (Gui.cpp). 그쪽은 자기 pipeline·자기 descriptor pool·자기 함수 로더를 들고 오고,
# 우리 swapchain과 무관한 MinImageCount/ImageCount를 요구한다 - 전부 이미 있는 것들이다.
#
# 그래서 volk 링크도, IMGUI_IMPL_VULKAN_* 매크로도 필요 없다. 남은 backend는 입력을
# 받는 GLFW 쪽 하나다.
target_link_libraries(imgui PUBLIC glfw)

set_target_properties(imgui PROPERTIES FOLDER "ThirdParty")
