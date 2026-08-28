# **폴더가 곧 규칙이다.** 각 폴더는 지킬 것이 하나씩 있고, 그것이 폴더의 존재 이유다.
#
#   NativeWrappers/  Vulkan 객체 하나씩 감싼 RAII 래퍼.
#                    **Public 헤더를 하나도 include하지 않는다** - RHI를 모른다.
#   Implementations/ Public 타입의 Vulkan 구현.
#                    **반드시 RHIxxx를 상속하고 밖으로 나간다.**
#                    (모듈명이 RHI라 폴더를 RHI/로 두면 "RHI 모듈의 헤더"처럼 읽힌다)
#   (루트)           조립 루트 · 아직 자리가 확정되지 않은 것 · 위아래가 다 보는 상수와 유틸.
#
# 한때 폴더를 안 나눴다. 그룹이 확정되지 않아 나누면 곧 다시 옮기게 되기 때문이었다.
# **폴더는 "이미 안정된 그룹"을 드러낼 때만 값을 한다.**
add_library(RHI STATIC
    # --- 공개 인터페이스: 상위가 아는 전부 ---
    #   만드는 쪽(DynamicRHI)과 기록하는 쪽(RHICommandContext)이 갈려 있다.
    #   언리얼도 FDynamicRHI / IRHICommandContext로 같은 자리에서 갈린다.
    Public/NativeWindowHandle.h
    Public/DynamicRHI.h
    Public/RHICommandContext.h
    Public/RHIViewport.h

    # --- 백엔드 무관: D3D12가 와도 여기 남는다 ---
    Private/RHILog.h

    # --- Vulkan 객체 래퍼: 각자 하나씩 소유하고 자기 파괴를 완결한다 ---
    Private/Vulkan/NativeWrappers/VulkanInstance.h
    Private/Vulkan/NativeWrappers/VulkanInstance.cpp
    Private/Vulkan/NativeWrappers/VulkanSurface.h
    Private/Vulkan/NativeWrappers/VulkanSurface.cpp
    Private/Vulkan/NativeWrappers/VulkanDevice.h
    Private/Vulkan/NativeWrappers/VulkanDevice.cpp
    Private/Vulkan/NativeWrappers/VulkanCommandPool.h
    Private/Vulkan/NativeWrappers/VulkanCommandPool.cpp
    Private/Vulkan/NativeWrappers/VulkanSwapchain.h
    Private/Vulkan/NativeWrappers/VulkanSwapchain.cpp

    # --- Public 타입의 Vulkan 구현: 밖으로 나간다 ---
    Private/Vulkan/Implementations/VulkanCommandContext.h
    Private/Vulkan/Implementations/VulkanCommandContext.cpp
    Private/Vulkan/Implementations/VulkanViewport.h
    Private/Vulkan/Implementations/VulkanViewport.cpp

    # --- 선택 정책 (클래스가 아니라 자유 함수) 과 공용 상수·유틸 ---
    Private/Vulkan/VulkanPhysicalDevice.h
    Private/Vulkan/VulkanPhysicalDevice.cpp
    Private/Vulkan/VulkanRequirements.h
    Private/Vulkan/VulkanResult.h

    # --- 조립 루트: 위의 것들을 조립하고 DynamicRHI를 구현한다 ---
    Private/Vulkan/VulkanRHI.h
    Private/Vulkan/VulkanRHI.cpp
)

# Public/만 다른 모듈에 노출한다. Private/(VulkanDevice 등)는 RHI 자기 자신만 볼 수 있다.
target_include_directories(RHI
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
    # Vulkan 안에서는 Vulkan/ 기준 경로로 include한다 - "NativeWrappers/VulkanDevice.h"처럼
    # **어느 그룹의 것인지가 include 문에 보이고**, 상대 경로(..)가 안 나온다.
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private/Vulkan
)

# volk는 RHI 구현 세부사항이라 PRIVATE로 링크한다.
# 이러면 volk의 PUBLIC 정의(VK_NO_PROTOTYPES 등)와 헤더 경로가 RHI 안에서만 보이고,
# RHI를 쓰는 쪽(sandbox)은 vulkan_core.h 26,000줄을 파싱하지 않는다.
target_link_libraries(RHI PRIVATE volk)

# 검증 레이어는 RHI 내부 구현 세부사항이라 PRIVATE로 준다.
# 항상 0 또는 1로 정의해서 #if가 오타에 조용히 넘어가지 않게 한다
# (#ifdef를 쓰면 매크로 이름을 틀려도 그냥 꺼진 채로 빌드된다).
if (LAMBDA_ENABLE_VULKAN_VALIDATION)
    target_compile_definitions(RHI PRIVATE LAMBDA_ENABLE_VULKAN_VALIDATION=1)
else()
    target_compile_definitions(RHI PRIVATE LAMBDA_ENABLE_VULKAN_VALIDATION=0)
endif()

target_compile_features(RHI PUBLIC cxx_std_20)

set_target_properties(RHI PROPERTIES FOLDER "Engine/Runtime")
