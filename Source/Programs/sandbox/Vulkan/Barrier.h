#pragma once

// Image layout transition
// ============================================================================
//
// A GPU image is laid out differently depending on what is about to touch it, and a
// barrier is what moves it between those layouts. It carries memory visibility with
// the move -- whether an earlier write is readable by a later read -- which is why
// both halves take a stage and an access mask.
//
// The transitions a frame makes are not listed here. They live in the passes that
// issue them, each next to the reason it is needed, and a copy of that list here would
// be one more thing to keep in step -- it already went stale once, describing two
// passes when there were four.

#include "Vulkan/Core.h"

// Input:  cmd, image, aspect, src/dst stage·access, old/new layout
// Effect: appends one barrier to cmd
//
// Eight arguments, none of them defaulted. A default settles on the safe value --
// ALL_COMMANDS -- and synchronization gets quietly heavier with nothing reporting it.
// What to wait for and what to make visible is the caller's answer, every time.
void RecordLayoutTransition(const VolkDeviceTable& vk, VkCommandBuffer cmd, VkImage image,
                            VkImageAspectFlags aspect,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                            VkImageLayout oldLayout, VkImageLayout newLayout) noexcept;
