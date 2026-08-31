#pragma once

#include "Vulkan/Commands.h"
#include "Vulkan/Device.h"

// Buffer - 메모리를 직접 다루는 첫 자리
// ============================================================================
//
// 앞의 것들(instance · device · swapchain · command pool)은 driver가 메모리를
// 알아서 잡아줬다. Buffer부터는 어떤 메모리에 놓을지 정해야 한다.
//
// VMA가 대신하는 것 (수동으로 해보고 바꿨다 - 4b71b59):
//   vkCreateBuffer / vkGetBufferMemoryRequirements / memoryProperties 조회 /
//   두 bitmask 대조 / vkAllocateMemory / vkBindBufferMemory  ->  vmaCreateBuffer 하나
//
// 줄어드는 것보다 중요한 것 둘:
//   1. vkAllocateMemory는 호출 횟수에 상한이 있다(maxMemoryAllocationCount, 보통 4096).
//      Buffer마다 부르면 바닥난다. VMA는 큰 덩어리를 잡고 그 안에서 잘라 준다.
//   2. Memory type 선택 정책(어떤 GPU에서 무엇이 빠른가)이 우리 코드에서 사라진다.
//
// 대가: volk와 같이 쓰려면 VmaVulkanFunctions를 손으로 채워야 한다(VK_NO_PROTOTYPES라
// VMA가 전역 심볼을 못 찾는다). CreateDevice에 그 코드가 있다.

struct Buffer {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    VkBuffer handle = VK_NULL_HANDLE;

    // VkDeviceMemory가 아니라 VmaAllocation이다. VMA가 큰 덩어리를 잡아두고 잘라
    // 주므로 이 handle은 "그 덩어리의 어느 구간"을 가리킨다. Offset도 VMA가 안다.
    VmaAllocation allocation = VK_NULL_HANDLE;

    // 매핑된 CPU 주소. HOST_VISIBLE + MAPPED_BIT이면 VMA가 채워준다. 아니면 nullptr.
    void* mapped = nullptr;

    VkDeviceSize size = 0;

    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
};

// Vertex 하나. GPU가 이걸 어떻게 읽을지는 pipeline의 vertex input이 정한다.
//
// Contract: shader의 layout(location=...) in과 짝이 맞아야 한다.
struct Vertex {
    // **world 좌표다. NDC가 아니다** - 화면 어디에 오는지는 vertex shader의 mvp가
    // 정한다. 그래서 z에 [0,1] 같은 제한이 없다. 카메라의 near~far 밖으로 나가면
    // 잘리는데, 그 판단은 투영이 깊이를 [0,1]로 옮긴 뒤에 일어난다.
    float position[3];   // vec3 -> VK_FORMAT_R32G32B32_SFLOAT
    float color[3];      // vec3 -> VK_FORMAT_R32G32B32_SFLOAT
};

// Buffer 생성 + 메모리 할당 + binding을 vmaCreateBuffer 한 번으로.
//
// memoryUsage: "누가 어떻게 쓰나"를 말하면 VMA가 memory type을 고른다.
//   AUTO              GPU가 주로 읽는다 -> DEVICE_LOCAL 선호
//   AUTO_PREFER_HOST  CPU가 자주 쓴다   -> HOST_VISIBLE 선호
// flags: HOST_ACCESS_SEQUENTIAL_WRITE는 "CPU가 순차로 쓴다",
//        MAPPED_BIT을 더하면 out->mapped에 주소가 들어온다.
bool CreateBuffer(const VulkanDevice& dev,
                  VkDeviceSize size,
                  VkBufferUsageFlags usage,
                  VmaMemoryUsage memoryUsage,
                  VmaAllocationCreateFlags flags,
                  Buffer* out) noexcept;

// CPU 데이터를 GPU 전용 메모리에 올린다 (staging 경유).
//
// GPU가 가장 빠르게 읽는 메모리(DEVICE_LOCAL)는 보통 CPU가 매핑할 수 없다. 그래서
// CPU가 쓸 수 있는 임시 buffer에 넣고 GPU에게 복사를 시킨다.
//
// Effect: 복사가 끝날 때까지 blocking한다. 초기화 경로라 기다려도 된다.
//
// usage를 인자로 받는다. 한동안 VERTEX_BUFFER_BIT을 안에 박아두고 이름도
// CreateVertexBuffer였는데, index buffer가 생기면서 갈렸다 - staging 경유 upload는
// 둘이 완전히 같고 usage 한 값만 다르다. TRANSFER_DST_BIT은 안에서 더한다.
bool CreateDeviceLocalBuffer(const VulkanDevice& dev,
                             const Commands& commands,
                             const void* data,
                             VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             Buffer* out) noexcept;
