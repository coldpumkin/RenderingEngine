add_library(Engine STATIC
    Public/Engine.h
    Private/Engine.cpp
)

target_include_directories(Engine
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

# RHI는 PUBLIC이다 - Engine.h가 DynamicRHI.h를 include하고 RHI()를 노출하므로
# Engine을 쓰는 쪽도 DynamicRHI 타입을 알아야 한다.
#
# GLFW는 PRIVATE이다 - Engine.h는 GLFWwindow를 전방 선언만 하므로
# Engine을 쓰는 쪽은 GLFW 헤더도, 링크도 필요 없다.
# 덕분에 창 라이브러리 교체가 Engine 안에서 끝난다.
target_link_libraries(Engine
    PUBLIC RHI
    PRIVATE glfw
)

target_compile_features(Engine PUBLIC cxx_std_20)

set_target_properties(Engine PROPERTIES FOLDER "Engine/Runtime")
