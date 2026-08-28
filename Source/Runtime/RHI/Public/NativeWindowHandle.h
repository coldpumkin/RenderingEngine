#pragma once

namespace LambdaEngine {

// 창을 만든 쪽이 채워서 넘긴다. RHI는 창의 수명에 관여하지 않는다.
//
// `void*`인 이유: `HWND`를 쓰면 이 헤더를 include하는 모든 파일이 `windows.h`를 끌고 온다.
// 두 개인 이유: `vkCreateWin32SurfaceKHR`이 `HINSTANCE`와 `HWND`를 둘 다 요구한다.
// (다른 플랫폼도 대개 2개다) — D42
//
// **`DynamicRHI.h`에서 떼어낸 이유**: 이건 인터페이스가 아니라 값 타입이고,
// `VulkanSurface` 같은 저수준 래퍼가 이것 하나 때문에 RHI 인터페이스 전체를
// 알게 되면 "래퍼는 RHI를 모른다"는 규칙이 깨진다.
struct NativeWindowHandle {
    void* instance = nullptr;   // Win32: HINSTANCE
    void* window   = nullptr;   // Win32: HWND
};

} // namespace LambdaEngine
