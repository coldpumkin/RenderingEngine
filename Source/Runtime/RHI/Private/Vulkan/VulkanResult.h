#pragma once

#include <volk.h>

namespace LambdaEngine {

// VkResult를 사람이 읽을 수 있는 이름으로. 실패 지점에서 원인을 남기기 위한 것이다.
//
// 왜 필요한가: vkCreateDevice 하나만 해도 실패 이유가 OUT_OF_DEVICE_MEMORY /
// EXTENSION_NOT_PRESENT / FEATURE_NOT_PRESENT / DEVICE_LOST 등으로 갈리고,
// 원인마다 대응이 완전히 다르다. "실패했다"만 남기면 디버깅이 불가능하다.
//
// 실패를 어떻게 처리할지(정책)는 호출자가 정하지만, **무엇이 왜 실패했는지는
// 실패 지점이 제일 잘 안다.** 그래서 기록은 여기서 하고 판단은 위로 넘긴다.
inline const char* ToString(VkResult result) noexcept {
    switch (result) {
    case VK_SUCCESS:                        return "VK_SUCCESS";
    case VK_NOT_READY:                      return "VK_NOT_READY";
    case VK_TIMEOUT:                        return "VK_TIMEOUT";
    case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
    default:                                return "VK_ERROR_<unmapped>";
    }
}

} // namespace LambdaEngine
