# VMA (Vulkan Memory Allocator) - GPU 메모리 할당자.
#
# **벤더링하지 않고 Vulkan SDK에 딸려 온 것을 쓴다.** volk와 다른 선택이고 이유가 있다:
#
#   volk를 벤더링한 이유는 "그 소스를 직접 읽는 게 학습 목표"였다. 함수 포인터를
#   어떻게 로드하는지가 배울 거리이고, 파일도 둘뿐이라 비용이 쌌다.
#
#   VMA는 헤더 하나에 2만 줄이고 내부 구현(할당 알고리즘, 조각화 처리)은 지금 우리
#   학습 대상이 아니다. 그리고 **SDK를 이미 필수 의존으로 요구하고 있으므로**
#   네트워크도 다운로드 대기도 없다 - 벤더링의 이유 셋 중 둘이 이미 충족된다.
#
# 헤더 경로는 Vulkan::Headers가 들고 온다 (SDK의 Include/ 아래 vma/가 있다).
#
# 버전은 SDK를 따라간다. 확인된 것: Vulkan SDK 1.4.350.0 -> VMA 3.3.0

set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

add_library(vma STATIC vma.cpp)

# volk를 PUBLIC으로 - vma.cpp가 volk.h를 include하고, 우리 코드도 같은 조건에서
# vk_mem_alloc.h를 include해야 한다 (VK_NO_PROTOTYPES가 켜진 상태로).
target_link_libraries(vma PUBLIC volk)

# 우리 코드가 vk_mem_alloc.h를 include할 때도 이 정의가 필요하다.
# 안 그러면 선언부가 "정적 함수를 쓴다"고 가정해서 구현부와 어긋난다.
target_compile_definitions(vma PUBLIC
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=0
)

set_target_properties(vma PROPERTIES FOLDER "ThirdParty")
