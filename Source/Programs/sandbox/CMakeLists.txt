add_executable(sandbox
    main.cpp
)

# 평평하게 쓰는 동안은 sandbox가 volk와 GLFW를 직접 든다.
# 중간 모듈이 없으므로 숨길 것도 없다.
target_link_libraries(sandbox PRIVATE volk glfw)

# 검증 레이어 on/off. 항상 0 또는 1로 정의해서 #if가 오타에 조용히 넘어가지 않게 한다.
if (LAMBDA_ENABLE_VULKAN_VALIDATION)
    target_compile_definitions(sandbox PRIVATE LAMBDA_ENABLE_VULKAN_VALIDATION=1)
else()
    target_compile_definitions(sandbox PRIVATE LAMBDA_ENABLE_VULKAN_VALIDATION=0)
endif()

set_target_properties(sandbox PROPERTIES FOLDER "Engine/Programs")
