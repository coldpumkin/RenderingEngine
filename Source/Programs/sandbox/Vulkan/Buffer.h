#pragma once

#include "Vulkan/Commands.h"
#include "Vulkan/Device.h"

// ============================================================================
// 9. 버퍼 - **메모리를 직접 다루는 첫 자리**
// ============================================================================
//
// 지금까지 만든 것(인스턴스·디바이스·스왑체인·커맨드 풀)은 전부 드라이버가 메모리를
// 알아서 잡아줬다. 버퍼부터는 **어떤 메모리에 놓을지 정해야 한다.**
//
// ---------------------------------------------------------------------------
// **VMA가 무엇을 대신하나** (수동으로 한 번 해보고 바꿨다 - git 4b71b59)
//
//   수동                                        VMA
//   ------------------------------------------  ---------------------------
//   vkCreateBuffer                              vmaCreateBuffer 하나
//   vkGetBufferMemoryRequirements                 "이 버퍼를 누가 쓰나"만 말하면
//   dev.memoryProperties 조회                     타입 선택 · 할당 · 바인딩을
//   두 비트마스크 대조 -> memoryTypeIndex          전부 안에서 한다
//   vkAllocateMemory
//   vkBindBufferMemory
//
// 줄어드는 것보다 중요한 것 둘:
//
//   1. **vkAllocateMemory는 호출 횟수에 상한이 있다** (maxMemoryAllocationCount,
//      보통 4096). 버퍼마다 부르면 금방 바닥난다. VMA는 큰 덩어리를 미리 잡고
//      그 안에서 잘라 주므로 버퍼 수와 할당 수가 분리된다.
//   2. 타입 선택 정책(어떤 GPU에서 무엇이 빠른가)이 우리 코드에서 사라진다.
//      VMA가 하드웨어별로 알고 있다.
//
// 대가: volk와 같이 쓰려면 VmaVulkanFunctions를 손으로 채워야 한다
// (VK_NO_PROTOTYPES라 VMA가 전역 심볼을 못 찾는다). CreateDevice에 그 코드가 있다.
// ---------------------------------------------------------------------------

struct Buffer {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 비소유 상태

    VkBuffer handle = VK_NULL_HANDLE;

    // **VkDeviceMemory가 아니라 VmaAllocation이다.**
    //
    // 전에는 버퍼마다 vkAllocateMemory를 불러 VkDeviceMemory를 하나씩 받았다.
    // VMA는 큰 덩어리를 미리 잡아두고 그 안에서 잘라 주므로, 이 핸들은
    // "그 덩어리의 어느 구간"을 가리킨다. 그래서 offset도 VMA가 안다.
    VmaAllocation allocation = VK_NULL_HANDLE;

    // 매핑된 CPU 주소. HOST_VISIBLE로 만들고 MAPPED_BIT을 주면 VMA가 채워준다.
    // 아니면 nullptr. **vkMapMemory/vkUnmapMemory를 매번 부르지 않아도 된다.**
    void* mapped = nullptr;

    VkDeviceSize size = 0;
    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
};

// 정점 하나. 셰이더의 layout(location=...) in 과 짝이 맞아야 한다.
//
// GPU가 이 구조체를 어떻게 읽을지는 파이프라인의 vertexInput이 정한다 -
// stride(한 정점의 크기)와 각 필드의 offset·format을 거기서 알려준다.
struct Vertex {
    // z가 생겼다. **뎁스 테스트가 비교하는 값이 이것**이고, 0(가까움)~1(멈) 범위다.
    // 이 범위 밖은 잘려 나간다 (파이프라인의 minDepth/maxDepth가 0~1이다).
    float position[3];   // vec3 -> VK_FORMAT_R32G32B32_SFLOAT
    float color[3];      // vec3 -> VK_FORMAT_R32G32B32_SFLOAT
};

// 버퍼 생성 + 메모리 할당 + 바인딩을 vmaCreateBuffer 한 번으로.
//
// **memoryUsage**: "이 버퍼를 누가 어떻게 쓰나"를 말하면 VMA가 메모리 타입을 고른다.
//   VMA_MEMORY_USAGE_AUTO                  GPU가 주로 읽는다 -> DEVICE_LOCAL 선호
//   VMA_MEMORY_USAGE_AUTO_PREFER_HOST      CPU가 자주 쓴다   -> HOST_VISIBLE 선호
// 전에는 이걸 우리가 VkMemoryPropertyFlags로 직접 말하고 대조까지 했다.
//
// **flags**: HOST_ACCESS_SEQUENTIAL_WRITE_BIT를 주면 "CPU가 순차로 쓴다"는 뜻이고,
//   MAPPED_BIT까지 주면 VMA가 매핑을 유지해서 out->mapped에 주소가 들어온다.
bool CreateBuffer(const VulkanDevice& dev,
                  VkDeviceSize size,
                  VkBufferUsageFlags usage,
                  VmaMemoryUsage memoryUsage,
                  VmaAllocationCreateFlags flags,
                  Buffer* out) noexcept;

// CPU 데이터를 GPU 전용 메모리에 올린다 (스테이징 경유).
//
// **왜 바로 못 올리나**: GPU가 가장 빠르게 읽는 메모리(DEVICE_LOCAL)는 보통 CPU가
// 매핑할 수 없다. 그래서 CPU가 쓸 수 있는 임시 버퍼(스테이징)에 넣고, GPU에게
// "저기서 여기로 복사해"라고 시킨다.
//
// 복사가 끝날 때까지 기다렸다가 스테이징을 버린다 - 초기화 경로라 기다려도 된다.
bool CreateVertexBuffer(const VulkanDevice& dev,
                        const Commands& commands,
                        const void* data,
                        VkDeviceSize size,
                        Buffer* out) noexcept;