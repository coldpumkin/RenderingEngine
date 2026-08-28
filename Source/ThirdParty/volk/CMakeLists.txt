# volk — Vulkan 메타 로더. 업스트림을 그대로 벤더링했다 (VERSION.md 참고).
#
# 업스트림 CMakeLists 대신 이 최소 버전을 쓰는 이유:
# 업스트림 것은 설치/export/테스트/샘플까지 다루느라 우리가 안 쓰는 분기가 대부분이다.
# volk 자체는 .c 하나 + .h 하나라서, 무엇이 왜 필요한지 전부 보이는 게 학습에 낫다.

# 서드파티라 우리 경고 정책(/W4)의 대상이 아니다. 우리가 고칠 코드가 아니니 경고를 봐도 할 게 없다.
#
# target_compile_options(volk PRIVATE /W0)으로 덮어쓰면 컴파일러가
# "D9025: '/W4'을(를) '/W0'(으)로 재정의합니다"를 매 빌드마다 뱉는다.
# 최상위 add_compile_options()는 디렉터리 속성으로 하위에 상속되므로,
# 덮어쓰는 대신 이 디렉터리에서 상속을 끊는 게 맞다.
# (add_library보다 먼저 와야 한다 — 타깃은 생성 시점의 디렉터리 옵션을 가져간다.)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

add_library(volk STATIC
    volk.c
    volk.h
)

target_include_directories(volk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Vulkan::Headers만 붙인다. Vulkan::Vulkan(로더 임포트 라이브러리)은 절대 붙이면 안 된다.
# 그걸 붙이는 순간 vulkan-1.lib에 정적 링크가 걸려서, vulkan-1.dll이 없는 기계에서
# 프로세스가 아예 시작조차 못 한다. volk의 존재 이유가 바로 그 정적 링크를 없애고
# 런타임에 LoadLibrary로 로더를 찾는 것이다 (없으면 volkInitialize()가 실패를 반환하고,
# 우리는 그걸 보고 정상적으로 에러 처리할 수 있다).
find_package(Vulkan REQUIRED)
target_link_libraries(volk PUBLIC Vulkan::Headers)

# volk.h가 스스로 VK_NO_PROTOTYPES를 정의하지만(volk.h:26), vulkan.h를 volk.h보다
# 먼저 include한 번역 단위가 생기면 그 시점엔 프로토타입이 이미 나와버린다.
# 컴파일 옵션으로 박아두면 include 순서와 무관하게 항상 안전하다.
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)

if (WIN32)
    target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set_target_properties(volk PROPERTIES FOLDER "ThirdParty")
