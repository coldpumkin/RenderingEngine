# 셋으로 나뉜 기준은 **초기화 vs 런타임**이다 (Vulkan.h 위 주석 참고).
add_executable(sandbox
    Vulkan.h          # 무엇이 있는가 (struct + 선언)
    VulkanSetup.cpp   # 어떻게 만들고 부수나 (한 번 돈다)
    main.cpp          # 한 프레임이 어떻게 도나 (매 프레임)
)

# 평평하게 쓰는 동안은 sandbox가 volk와 GLFW를 직접 든다.
# 중간 모듈이 없으므로 숨길 것도 없다.
target_link_libraries(sandbox PRIVATE volk glfw vma)

# std::fopen이 MSVC에서 C4996(deprecated)로 경고를 낸다. fopen_s는 표준이 아니라
# 이 파일이 MSVC 전용이 되고, 여기서는 셰이더 파일 하나를 읽을 뿐이라 얻는 게 없다.
target_compile_definitions(sandbox PRIVATE _CRT_SECURE_NO_WARNINGS)

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

foreach(SHADER triangle.vert triangle.frag)
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
    Shaders/triangle.vert
    Shaders/triangle.frag
)
set_source_files_properties(Shaders/triangle.vert Shaders/triangle.frag
                            PROPERTIES HEADER_FILE_ONLY TRUE)

set_target_properties(sandbox sandbox_shaders PROPERTIES FOLDER "Engine/Programs")
