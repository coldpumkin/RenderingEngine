# **개념당 한 쌍.** 언리얼 VulkanRHI와 같은 축이다 - 자원이 늘어도 기존 파일이
# 안 자라고 새 쌍이 하나 생긴다. 추상화 층은 없다 (struct + 자유 함수뿐).
#
# 층이 둘이고 의존이 한 방향으로만 흐른다. **Vulkan/ 아래의 어떤 헤더도 위 두 줄을
# include하지 않는다** - 그것이 이 선의 전부다:
#
#   main.cpp  ->  Passes . Vertex  ->  Vulkan/*
#
# Vulkan/ 안에서도 한 방향이다:
#   Core <- Instance <- Device <- { Swapchain, Commands, Pipeline, Buffer }
#                                    ^                      ^
#                                  Window                 Frame (Window + Commands를 쓴다)
#                          (서피스는 Instance, 스왑체인을 품는다)
add_executable(sandbox
    Config.h                # 손잡이 (컴파일 상수)

    Vulkan/Core.h           # 두 함수 테이블 · 요구사항 · RAII 규약
    Vulkan/Instance.h    Vulkan/Instance.cpp
    Vulkan/Device.h      Vulkan/Device.cpp      # + 큐 패밀리 · 큐 · GPU 선택 · VMA 할당자
    Vulkan/Window.h      Vulkan/Window.cpp      # 창 시스템 · 창 · 서피스
    Vulkan/Swapchain.h   Vulkan/Swapchain.cpp
    Vulkan/Image.h       Vulkan/Image.cpp       # image + view (render target과 texture가 같이 쓴다)
    Vulkan/Attachments.h Vulkan/Attachments.cpp  # 무엇에 그리는지의 명세 (포맷·샘플 수)
    Vulkan/Texture.h     Vulkan/Texture.cpp     # 셰이더가 읽는 image
    Vulkan/Barrier.h     Vulkan/Barrier.cpp     # 이미지 레이아웃 전이
    Vulkan/Descriptors.h Vulkan/Descriptors.cpp # 셰이더가 이미지를 읽게 하는 장치
    Vulkan/Commands.h    Vulkan/Commands.cpp    # 커맨드 풀 (큐 패밀리마다)
    Vulkan/Frame.h       Vulkan/Frame.cpp       # 프레임 슬롯 · 프레임 타겟 · 여닫기
    Vulkan/Shader.h      Vulkan/Shader.cpp      # .spv 읽기 + SPIR-V reflection
    Vulkan/Pipeline.h    Vulkan/Pipeline.cpp
    Vulkan/Buffer.h      Vulkan/Buffer.cpp
    Vulkan/Mesh.h        Vulkan/Mesh.cpp        # 한 draw가 늘 같이 bind하는 버퍼 둘

    Vertex.h             Vertex.cpp        # 우리 정점 형식 + 그 layout 선언
    Passes.h             Passes.cpp        # scene / post-process pass, DrawItem, 기록

    main.cpp                # 무엇을 만들고 무엇을 프레임에 건네나
)

# 자기 폴더를 include 루트로 - "Vulkan/Device.h"처럼 어느 그룹인지가 include에 보인다.
target_include_directories(sandbox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 평평하게 쓰는 동안은 sandbox가 volk와 GLFW를 직접 든다.
# 중간 모듈이 없으므로 숨길 것도 없다.
target_link_libraries(sandbox PRIVATE volk glfw vma glm spirv-reflect cgltf stb)

# std::fopen이 MSVC에서 C4996(deprecated)로 경고를 낸다. fopen_s는 표준이 아니라
# 이 파일이 MSVC 전용이 되고, 여기서는 셰이더 파일 하나를 읽을 뿐이라 얻는 게 없다.
target_compile_definitions(sandbox PRIVATE _CRT_SECURE_NO_WARNINGS)

# 에셋은 셰이더와 다르게 실행 파일 옆으로 복사하지 않는다. 셰이더는 컴파일 산출물이라
# 빌드 트리에 있어야 하지만 에셋은 소스이고, Sponza만 9.3MB에 텍스처까지 오면 50MB라
# 빌드마다 복사할 이유가 없다. 대신 소스 경로를 박는다.
#
# 이 경로가 없어도 정상이다 - 에셋은 gitignore라 각자 받고, main은 없으면 MakeSphere로
# 돌아간다.
target_compile_definitions(sandbox PRIVATE
    LAMBDA_ASSET_ROOT="${CMAKE_SOURCE_DIR}/Assets")

# 검증 레이어 on/off. 항상 0 또는 1로 정의해서 #if가 오타에 조용히 넘어가지 않게 한다.
if (LAMBDA_ENABLE_VULKAN_VALIDATION)
    target_compile_definitions(sandbox PRIVATE LAMBDA_ENABLE_VULKAN_VALIDATION=1)
else()
    target_compile_definitions(sandbox PRIVATE LAMBDA_ENABLE_VULKAN_VALIDATION=0)
endif()

# ---- 셰이더: GLSL -> SPIR-V ----
#
# **빌드 시점에 컴파일한다** (런타임 아님). glslc는 Vulkan SDK에 딸려 오므로
# 새 의존성이 없다. 런타임 컴파일(shaderc/glslang 링크)은 셰이더를 실행 중에
# 고쳐야 할 때 의미가 있고, 지금은 그럴 일이 없다.
find_program(GLSLC_EXE glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)

# **OUTPUT에는 제너레이터 표현식($<TARGET_FILE_DIR:...>)을 쓸 수 없다.**
# configure 시점에 타깃이 아직 없어서 CMake가 평가하지 못한다. 그래서 고정 경로로
# 만들고, 실행 파일 옆으로는 POST_BUILD에서 복사한다 (거기선 표현식이 허용된다).
set(SHADER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders")
set(SPV_FILES "")

foreach(SHADER mesh.vert mesh.frag fullscreen.vert fullscreen.frag)
    set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/${SHADER}")
    set(SPV "${SHADER_BUILD_DIR}/${SHADER}.spv")
    add_custom_command(
        OUTPUT  "${SPV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_BUILD_DIR}"
        COMMAND "${GLSLC_EXE}" "${SRC}" -o "${SPV}"
        DEPENDS "${SRC}"
        COMMENT "glslc ${SHADER}"
        VERBATIM
    )
    list(APPEND SPV_FILES "${SPV}")
endforeach()

# 별도 타깃으로 두고 sandbox가 의존하게 한다. 셰이더만 고쳐도 다시 컴파일된다.
add_custom_target(sandbox_shaders DEPENDS ${SPV_FILES})
add_dependencies(sandbox sandbox_shaders)

# 실행 파일은 상대 경로 "Shaders/..."로 찾으므로 실행 파일 옆에 있어야 한다.
add_custom_command(TARGET sandbox POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${SHADER_BUILD_DIR}" "$<TARGET_FILE_DIR:sandbox>/Shaders"
    COMMENT "copying shaders next to sandbox.exe"
    VERBATIM
)

# VS에서 셰이더 원본이 보이게 (빌드 대상은 아니다)
target_sources(sandbox PRIVATE
    Shaders/mesh.vert
    Shaders/mesh.frag
    Shaders/fullscreen.vert
    Shaders/fullscreen.frag
)
set_source_files_properties(Shaders/mesh.vert Shaders/mesh.frag
                            PROPERTIES HEADER_FILE_ONLY TRUE)

set_target_properties(sandbox sandbox_shaders PROPERTIES FOLDER "Engine/Programs")
