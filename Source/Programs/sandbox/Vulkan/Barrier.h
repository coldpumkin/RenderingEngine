#pragma once

// ============================================================================
// 이미지 레이아웃 전이 - **언제 무엇이 보이는가**
// ============================================================================
//
// GPU 이미지는 **용도마다 내부 배치가 다르다.** "렌더 타겟으로 쓸 때 빠른 배치"와
// "전송원으로 읽을 때의 배치"와 "화면에 내보낼 때의 배치"가 다르고, 그 사이를 명시적으로
// 바꿔줘야 한다. 배리어는 그 전환과 함께 **메모리 가시성**(앞의 쓰기가 뒤의 읽기에
// 보이는가)도 처리한다.
//
// **왜 main.cpp에서 나왔나**: 오프스크린이 들어오면서 한 프레임의 전이가 다섯이 됐다.
//   1. 우리 색 이미지   UNDEFINED         -> COLOR_ATTACHMENT_OPTIMAL   (그리기 전)
//   2. 우리 뎁스        UNDEFINED         -> DEPTH_ATTACHMENT_OPTIMAL
//   3. 우리 색 이미지   COLOR_ATTACHMENT  -> TRANSFER_SRC_OPTIMAL       (복사 전)
//   4. 스왑체인 이미지  UNDEFINED         -> TRANSFER_DST_OPTIMAL
//   5. 스왑체인 이미지  TRANSFER_DST      -> PRESENT_SRC_KHR            (내보내기 전)
// 전이 하나하나에 스테이지·액세스 조합이 붙어서 RecordFrame이 배리어 코드로 덮였다.

#include "Vulkan/Core.h"

// 인자가 여덟이다. **줄이지 않는 이유**: 스테이지와 액세스는 전이마다 다르고,
// 기본값을 주면 "안전한 값"(ALL_COMMANDS)으로 수렴해 동기화가 조용히 과해진다.
// 무엇을 기다리고 무엇을 보이게 하는지는 호출자가 매번 정해야 하는 것이 맞다.
void RecordLayoutTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd, VkImage image,
                            VkImageAspectFlags aspect,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                            VkImageLayout oldLayout, VkImageLayout newLayout) noexcept;
