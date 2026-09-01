# SPIRV-Reflect - SPIR-V에서 shader의 인터페이스를 읽어낸다.
#
# 벤더링한 이유는 SDK가 헤더 경로에 안 올려주기 때문이다. VMA는 SDK의 Include/
# 아래에 있어 링크만 하면 됐지만, 이것은 Source/SPIRV-Reflect/에 소스로만 있고
# .c를 우리가 빌드해야 한다.
#
# 버전은 VERSION.md 참고. C 라이브러리라 최상위 project()의 C가 여기도 쓰인다.

set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

add_library(spirv-reflect STATIC spirv_reflect.c)

# spirv_reflect.c가 "spirv/unified1/spirv.h"를 찾는다.
target_include_directories(spirv-reflect PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(spirv-reflect PROPERTIES FOLDER "ThirdParty")
