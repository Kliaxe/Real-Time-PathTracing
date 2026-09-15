#pragma once

#include <span>

#include <volk.h>

namespace rtpt
{

struct Image;

// AccessScope
// How a resource is used on one side of a synchronization2 barrier: the pipeline stages that touch it and the kind of access.
// Pairing the two keeps callers from filling the stage and access masks of a barrier inconsistently.

struct AccessScope
{
  // Pipeline stages that use the resource.
  VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
  // Read or write access those stages perform.
  VkAccessFlags2        access = VK_ACCESS_2_NONE;
};

// ImageAccessScope
// An AccessScope plus the state only images carry: their layout and the queue family that owns them.

struct ImageAccessScope
{
  // Stages and access for the image.
  AccessScope   access {};
  // Layout the image is in, or is transitioned to, on this side of the barrier.
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Owning queue family. VK_QUEUE_FAMILY_IGNORED on both sides means no ownership transfer.
  uint32_t      queueFamily = VK_QUEUE_FAMILY_IGNORED;
};

[[nodiscard]] VkMemoryBarrier2 MemoryBarrier(AccessScope source, AccessScope destination);
[[nodiscard]] VkBufferMemoryBarrier2 BufferBarrier(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, AccessScope source, AccessScope destination, uint32_t sourceQueueFamily = VK_QUEUE_FAMILY_IGNORED, uint32_t destinationQueueFamily = VK_QUEUE_FAMILY_IGNORED);
[[nodiscard]] VkImageMemoryBarrier2 ImageBarrier(VkImage image, VkImageSubresourceRange range, ImageAccessScope source, ImageAccessScope destination);

// Records any mix of barriers as one vkCmdPipelineBarrier2 call.
void CmdBarriers(VkCommandBuffer commandBuffer, std::span<const VkMemoryBarrier2> memoryBarriers = {}, std::span<const VkBufferMemoryBarrier2> bufferBarriers = {}, std::span<const VkImageMemoryBarrier2> imageBarriers = {}, VkDependencyFlags dependencyFlags = 0);

// Single-barrier shorthands for the common case.
void CmdMemoryBarrier(VkCommandBuffer commandBuffer, AccessScope source, AccessScope destination);
void CmdBufferBarrier(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, AccessScope source, AccessScope destination);
void CmdImageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageSubresourceRange range, ImageAccessScope source, ImageAccessScope destination);

// StorageImageWriteOrdering
// How TransitionStorageImageForWrite treats an image the command buffer already left in GENERAL.
// PathTracer and ReSTIR PT were written against different rules, so each call site names the one it relies on.

enum class StorageImageWriteOrdering
{
  // Always record a barrier. An image that already holds contents names ALL_COMMANDS as the earlier writer, which orders this access after any previous write.
  eOrderAfterPreviousWrites,
  // Record a barrier only for the first transition after creation; an image already tracked as GENERAL gets none.
  eFirstTransitionOnly,
};

// Moves one storage image to GENERAL for shader read/write and records GENERAL as its tracked layout. A null image is ignored.
void TransitionStorageImageForWrite(VkCommandBuffer commandBuffer, Image& image, VkPipelineStageFlags2 destinationStages, StorageImageWriteOrdering ordering);

}  // namespace rtpt
