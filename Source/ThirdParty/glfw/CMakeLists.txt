# GLFW - 창 생성과 입력. volk와 달리 FetchContent로 가져온다.
#
# Decisions.md D11(서드파티는 벤더링)에서 벗어나는 것이고, 근거는 이렇다:
# D11의 이유 셋 중 가장 큰 것은 "그 소스를 직접 읽는 게 학습 목표에 포함된다"였다.
# volk는 실제로 그렇다 (함수 포인터를 어떻게 로드하는지가 배울 거리다).
# GLFW 내부는 우리 학습 대상이 아니다 - 창을 띄우는 수단일 뿐이다.
# 그리고 volk는 파일 2개였지만 GLFW는 수십 개라 벤더링 비용이 전혀 다르다.
#
# 대가: 최초 configure 때 네트워크가 필요하다. 그 뒤로는 캐시된다.

# 상위에서 내려온 컴파일 옵션(/W4)을 여기서 끊는다.
# FetchContent_MakeAvailable은 이 디렉터리의 자식으로 add_subdirectory를 하므로,
# **MakeAvailable보다 먼저** 끊어야 GLFW 타깃이 깨끗한 상태로 만들어진다.
#
# volk 때처럼 target_compile_options(/W0)로 덮으면 안 된다 - GLFW는 자기 CMake에서
# /W3을 따로 넣기 때문에 /W4 -> /W3 -> /W0 삼중 재정의가 되어 D9025가 파일마다 뜬다.
# 단, /utf-8은 남긴다. 전부 지우면 MSVC가 소스를 시스템 코드페이지(CP949)로 읽어서
# GLFW 헤더의 비ASCII 문자에 C4819가 뜬다. 경고 정책은 끊되 인코딩 해석은 유지해야 한다.
if (MSVC)
    set_directory_properties(PROPERTIES COMPILE_OPTIONS "/utf-8")
else()
    set_directory_properties(PROPERTIES COMPILE_OPTIONS "")
endif()

include(FetchContent)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)

# 우리가 안 쓰는 것들을 끈다. 켜두면 빌드 시간만 늘어난다.
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw)

if (TARGET glfw)
    set_target_properties(glfw PROPERTIES FOLDER "ThirdParty")
endif()
if (TARGET update_mappings)
    set_target_properties(update_mappings PROPERTIES FOLDER "ThirdParty")
endif()
