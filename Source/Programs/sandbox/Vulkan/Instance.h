#pragma once

#include "Vulkan/Core.h"

// ============================================================================
// 1. 인스턴스
// ============================================================================
// 인스턴스 층. 셋이 같이 태어나고 같이 죽는다.
//
// **table과 handle을 한 묶음에 두는 이유**: Vulkan 호출은 예외 없이 두 테이블 중
// 하나로 갈린다(실측: 인스턴스 26곳 / 디바이스 30곳). 우리가 정한 선이 아니라
// API 자체의 선이고, 핸들과 테이블이 한 곳에서 나와야 섞일 수 없다.
//
// 다만 **필요 범위는 서로 다르다.** 테이블은 거의 모든 곳에 필요하지만 핸들은
// 만들고 부수는 곳에만 필요하다 - 물리 디바이스 조회는 gpu로 디스패치하지
// 인스턴스로 하지 않기 때문이다. 함께 넘기는 비용은 참조 하나라 묶는 쪽이 낫다.
//
// **아직 소멸자가 없다.** 정리는 main() 끝에 모여 있고, 그 순서가 실제로 아플 때
// RAII로 옮긴다. 그때 이 struct의 모양은 안 바뀐다 - 소멸자만 붙는다.
struct VulkanInstance {
    VolkInstanceTable table{};
    VkInstance handle = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    // **자기 힘으로 파괴한다.** 파괴에 필요한 것(테이블 + 핸들)을 이미 자기가 들고 있다.
    VulkanInstance() = default;
    ~VulkanInstance();
    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;
};

// 로더 확인 -> 인스턴스 -> 함수 테이블 -> 디버그 메신저.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
//
// **out 파라미터가 없어졌다.** 셋이 한 묶음이 되니 반환값 하나면 된다 -
// out 파라미터는 애초에 "이것들은 같이 나온다"는 신호였다.
// 로더 확인 -> 인스턴스 -> 함수 테이블 -> 디버그 메신저.
// 실패하면 handle이 VK_NULL_HANDLE인 채로 돌아온다.
bool CreateInstance(VulkanInstance* out) noexcept;