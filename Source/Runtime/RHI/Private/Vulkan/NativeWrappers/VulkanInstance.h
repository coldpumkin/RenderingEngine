#pragma once

#include <volk.h>

#include <memory>

namespace LambdaEngine {

// VkInstance와 그 인스턴스의 함수 테이블을 소유한다. 디버그 메신저도 여기 있다
// (수명이 인스턴스와 같아서 나눌 근거가 없다).
//
// VulkanDevice와 분리한 기준(D24):
//   수명       - 인스턴스는 프로세스당 하나, 디바이스는 device lost 시 재생성 대상
//   변하는 이유 - 인스턴스는 검증 레이어/인스턴스 확장, 디바이스는 GPU 기능/큐/디바이스 확장
class VulkanInstance {
public:
    // volk 로더 초기화 + VkInstance 생성 (+ 가능하면 검증 레이어). 실패하면 nullptr.
    // noexcept인데 vector/new를 쓴다. bad_alloc이면 terminate되고, 그게 옳다 (F.6).
    static std::unique_ptr<VulkanInstance> Create() noexcept;

    ~VulkanInstance();

    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;
    VulkanInstance(VulkanInstance&&) = delete;
    VulkanInstance& operator=(VulkanInstance&&) = delete;

    VkInstance Handle() const noexcept { return instance_; }

    // 이 인스턴스의 함수들. 핸들과 함수가 같은 객체에서 나오므로 섞일 수 없다.
    const VolkInstanceTable& Table() const noexcept { return table_; }

private:
    VulkanInstance(VkInstance instance,
                   const VolkInstanceTable& table,
                   VkDebugUtilsMessengerEXT messenger) noexcept;

    VkInstance instance_ = VK_NULL_HANDLE;

    // 전역(volkLoadInstanceOnly가 채우는 것) 대신 테이블을 쓰는 이유는 성능이 아니다.
    // 인스턴스 함수는 전부 초기화 전용이라 매 프레임 경로에 없다. 이유는 D2(전역 상태
    // 지양)와의 정합, 그리고 핸들과 함수가 한 객체에서 나온다는 것이다.
    //
    // 한계: volkLoadDeviceTable이 전역 vkGetDeviceProcAddr에 의존하므로
    // volkLoadInstanceOnly를 뺄 수 없다. 인스턴스 레벨에서는 강제가 아니라 규약이다.
    VolkInstanceTable table_{};

    // 검증 레이어를 못 켰으면 VK_NULL_HANDLE로 남는다. 이건 "빈 껍데기"와 다르다 -
    // 없어도 정상 동작하는 선택적 기능이라 없음이 유효한 상태다.
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
};

} // namespace LambdaEngine
