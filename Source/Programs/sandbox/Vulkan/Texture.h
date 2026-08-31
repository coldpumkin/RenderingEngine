#pragma once

// Texture - shader가 읽는 image
// ============================================================================
//
// Render target과 같은 image인데 방향이 반대다. 그쪽은 우리가 **그려 넣고**, 이쪽은
// shader가 **읽는다**. 그래서 usage와 layout이 다르고, CPU에서 올린 데이터가 있다.
//
// 만드는 절차가 vertex buffer와 거의 같다 - staging buffer에 넣고 GPU에게 복사를
// 시킨다. 다른 것은 **layout 전이가 앞뒤로 붙는다**는 점이다:
//
//   UNDEFINED -> TRANSFER_DST_OPTIMAL   복사를 받을 수 있는 상태로
//   (vkCmdCopyBufferToImage)
//   TRANSFER_DST -> SHADER_READ_ONLY    shader가 읽을 수 있는 상태로
//
// Buffer에는 이 단계가 없다. Image는 driver가 내부 배치를 바꿔가며 쓰기 때문이다.
//
// descriptor set은 pool에서 나오고 개별 반납이 없어서 소멸자가 안 지운다.

#include "Vulkan/Commands.h"
#include "Vulkan/Descriptors.h"
#include "Vulkan/Image.h"

struct Texture {
    const VulkanDevice* dev = nullptr;   // 파괴에 필요한 non-owning 상태

    Image image;

    // 이 texture가 binding 0에 걸린 set. binding 1은 다른 texture라 짝을 여기서
    // 못 정한다 - 호출자가 AllocateImageSet으로 채운다. pool이 죽을 때 같이 사라진다.
    VkDescriptorSet set = VK_NULL_HANDLE;

    Texture() = default;
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
};

// 파일에서 읽지 않고 코드로 만든다.
//
// 이미지 로더(stb_image 같은)를 들이려면 Spike에서 단독 검증부터 해야 하고, 지금
// 보려는 것은 "shader가 image를 읽는 경로"지 파일 포맷이 아니다. checkerboard면
// uv가 맞는지 · 필터가 도는지 · 좌우상하가 안 뒤집혔는지가 전부 눈에 보인다.
// 둘이 된 이유: 물체마다 다른 texture를 붙여보려고. 그게 draw마다 무엇이 달라지는지를
// 드러낸다 - descriptor set이 하나뿐일 때는 pass 앞에서 한 번 bind하고 끝이었다.
//
// 나란히 놓고 보면 실제로 다른 것은 **픽셀을 만드는 6줄과 로그 이름**뿐이다.
// staging · image · layout 전이 둘 · 복사는 한 글자도 안 다르다.
// 그래서 .cpp에서 그 공통부를 static 함수 하나로 접었다.
bool CreateCheckerTexture(const VulkanDevice& dev,
                          const Commands& commands,
                          Texture* out) noexcept;

// 세로 줄무늬. checker와 눈으로 구분되는 것이 목적의 전부다.
bool CreateStripeTexture(const VulkanDevice& dev,
                         const Commands& commands,
                         Texture* out) noexcept;
