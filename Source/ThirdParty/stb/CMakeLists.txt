# stb_image - PNG/JPEG 디코더.
#
# 헤더 하나에 선언과 구현이 다 들어 있고, STB_IMAGE_IMPLEMENTATION을 정의한 TU에서만
# 구현이 펼쳐진다. 그 TU가 stb_image.c다 (cgltf.c · vma.cpp와 같은 구조).
#
# 폴더 이름이 stb_image가 아니라 stb인 이유: 업스트림이 한 저장소에 여러 단일 헤더를
# 담고 있어서, stb_image_write나 stb_truetype이 나중에 오면 여기 옆에 놓인다.
# 지금은 stb_image.h 하나뿐이다.
#
# 버전은 VERSION.md 참고. C 라이브러리라 최상위 project()의 C가 여기도 쓰인다.

set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

add_library(stb STATIC stb_image.c)

target_include_directories(stb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# stbi_load가 fopen을 쓴다. 업스트림을 고치지 않고 그대로 두려면 필요하다.
# PRIVATE인 이유: 우리 코드는 stb의 함수만 부르지 fopen을 직접 안 쓴다.
if (MSVC)
    target_compile_definitions(stb PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(stb PROPERTIES FOLDER "ThirdParty")
