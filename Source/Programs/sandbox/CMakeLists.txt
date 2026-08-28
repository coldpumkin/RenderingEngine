add_executable(sandbox
    main.cpp
)

# Engine만 링크한다. RHI는 Engine이 PUBLIC으로 물고 오고, GLFW도 volk도 아예 안 보인다.
# 한때 여기에 RHI의 Private 경로와 volk를 여는 세 줄이 있었다 - 한 프레임을 raw Vulkan으로
# 펼쳐보기 위한 임시 구멍이었고, 인터페이스를 정하면서 닫혔다.
target_link_libraries(sandbox PRIVATE Engine)

set_target_properties(sandbox PROPERTIES FOLDER "Engine/Programs")
