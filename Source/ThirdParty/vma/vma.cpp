// VMA(Vulkan Memory Allocator)의 **구현 번역 단위**.
//
// VMA는 헤더 하나에 선언과 구현이 다 들어 있고(약 2만 줄), VMA_IMPLEMENTATION을
// 정의한 곳에서만 구현이 펼쳐진다. 그 자리를 여기 하나로 고정하는 이유:
// 우리 코드에서 include할 때마다 2만 줄을 다시 컴파일하지 않기 위해서다.
//
// **볼크와 같이 쓰기 때문에 해야 하는 것들:**
//   VMA_STATIC_VULKAN_FUNCTIONS 0   전역 vk* 심볼을 직접 부르지 않는다.
//                                   VK_NO_PROTOTYPES라 그런 심볼이 아예 없다.
//   VMA_DYNAMIC_VULKAN_FUNCTIONS 0  VMA가 스스로 로드하지도 않는다.
//                                   **우리가 VmaVulkanFunctions를 채워서 넘긴다** -
//                                   그래야 우리 디바이스 테이블과 같은 포인터를 쓴다.
//
// 이 둘을 안 끄면 링크 에러가 나거나(전역 심볼 없음), VMA가 자기 나름대로 로드해서
// 우리 테이블과 다른 포인터를 쓰게 된다.

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION

// volk가 먼저 와야 VK_NO_PROTOTYPES가 켜진 상태로 vulkan 헤더가 들어간다.
#include <volk.h>
#include <vma/vk_mem_alloc.h>
