cmake_minimum_required(VERSION 3.24)
# volk.c 때문에 C도 필요하다 (우리 코드는 전부 C++).
project(RenderingEngine LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    # /Zc:preprocessor - C++20 표준 준수 전처리기.
    #
    # -std:c++20을 켜도 MSVC의 전처리기는 기본적으로 레거시 모드로 남는다.
    # 그 상태에서는 __VA_OPT__(C++20) 같은 표준 기능이 컴파일 에러가 난다 (실제로 겪었음).
    # 언어 표준과 전처리기 표준이 따로 노는 건 MSVC 고유의 함정이다.
    #
    # 서드파티(volk)에는 적용되지 않는다 - 그쪽에서 상속을 끊어놨다.
    add_compile_options(/utf-8 /W4 /Zc:preprocessor)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# 검증 레이어: 드라이버가 조용히 넘어가는 잘못된 Vulkan 사용을 잡아준다.
# 드로우콜마다 검사가 들어가서 런타임 비용이 크므로 Release 프리셋을 만들면 거기서 끈다.
#
# 기본값을 CMAKE_BUILD_TYPE으로 판단하지 않는 이유:
# 멀티 컨피그 제너레이터(Visual Studio, Ninja Multi-Config)에서는 configure 시점에
# CMAKE_BUILD_TYPE이 비어 있어서 조건이 조용히 거짓이 된다. 그러면 아무 경고 없이
# 검증이 꺼진 채로 빌드된다. 영리한 자동 판단보다 명시적인 기본값이 낫다.
# (어차피 아래 message(STATUS)로 매 configure마다 값이 찍힌다.)
#
# Vulkan SDK가 없는 기계에서는 레이어를 못 찾아도 그냥 끄고 진행한다 (실패로 치지 않음).
option(LAMBDA_ENABLE_VULKAN_VALIDATION
       "Enable Vulkan validation layers (VK_LAYER_KHRONOS_validation)"
       ON)
message(STATUS "Vulkan validation layers: ${LAMBDA_ENABLE_VULKAN_VALIDATION}")

add_subdirectory(Source/ThirdParty/volk)
add_subdirectory(Source/ThirdParty/glfw)

add_subdirectory(Source/Runtime/RHI)
add_subdirectory(Source/Runtime/Engine)
add_subdirectory(Source/Programs/sandbox)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT sandbox)
