# cgltf - glTF 2.0 파서.
#
# 헤더 하나에 선언과 구현이 다 들어 있고, CGLTF_IMPLEMENTATION을 정의한 TU에서만
# 구현이 펼쳐진다. 그 TU가 cgltf.c다 (vma.cpp와 같은 구조).
#
# FetchContent가 아니라 벤더링인 이유는 방침 그대로다 - 파일이 하나뿐이라 복사가
# 제일 싸고, 네트워크 없이 configure되고, 소스를 직접 읽는 것도 학습 목표에 든다.
# GLFW만 예외인 것은 빌드 시스템이 커서였지 여기엔 해당이 없다.
#
# 버전은 VERSION.md 참고. C 라이브러리라 최상위 project()의 C가 여기도 쓰인다.

set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

add_library(cgltf STATIC cgltf.c)

target_include_directories(cgltf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# cgltf가 sscanf/strcpy 계열을 쓴다. 업스트림을 고치지 않고 그대로 두려면 이 정의가
# 필요하다. PUBLIC이 아니라 PRIVATE인 이유: 우리 코드는 그 함수들을 안 쓰므로
# 경고를 끌 이유가 없다.
if (MSVC)
    target_compile_definitions(cgltf PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(cgltf PROPERTIES FOLDER "ThirdParty")
