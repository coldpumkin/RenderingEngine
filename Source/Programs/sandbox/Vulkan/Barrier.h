#pragma once

// Image layout transition
// ============================================================================
//
// GPU image는 용도마다 내부 배치가 다르다. Barrier가 그 전환과 함께 memory
// visibility(앞의 write가 뒤의 read에 보이는가)도 처리한다.
//
// 한 frame의 transition 다섯:
//   1. 우리 color   UNDEFINED         -> COLOR_ATTACHMENT_OPTIMAL   (pass 1 전)
//   2. 우리 depth   UNDEFINED         -> DEPTH_ATTACHMENT_OPTIMAL
//   3. 우리 color   COLOR_ATTACHMENT  -> SHADER_READ_ONLY_OPTIMAL   (pass 2가 읽는다)
//   4. swapchain    UNDEFINED         -> COLOR_ATTACHMENT_OPTIMAL   (pass 2가 그린다)
//   5. swapchain    COLOR_ATTACHMENT  -> PRESENT_SRC_KHR            (present 전)

#include "Vulkan/Core.h"

// Input:  cmd, image, aspect, src/dst stage·access, old/new layout
// Effect: barrier 하나가 cmd에 append된다
//
// 인자 여덟을 줄이지 않는 이유: 기본값을 주면 안전한 값(ALL_COMMANDS)으로 수렴해
// 동기화가 조용히 과해진다. 무엇을 기다리고 무엇을 보이게 할지는 호출자가 매번 정한다.
void RecordLayoutTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd, VkImage image,
                            VkImageAspectFlags aspect,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                            VkImageLayout oldLayout, VkImageLayout newLayout) noexcept;
