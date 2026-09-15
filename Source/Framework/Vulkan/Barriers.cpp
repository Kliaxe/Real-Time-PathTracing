#include "Barriers.h"

#include <array>

#include "GpuResources.h"

namespace rtpt
{

VkMemoryBarrier2 MemoryBarrier(AccessScope source, AccessScope destination)
{
  return VkMemoryBarrier2 {
    .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
    .srcStageMask  = source.stages,
    .srcAccessMask = source.access,
    .dstStageMask  = destination.stages,
    .dstAccessMask = destination.access,
  };
}

VkBufferMemoryBarrier2 BufferBarrier(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, AccessScope source, AccessScope destination, uint32_t sourceQueueFamily, uint32_t destinationQueueFamily)
{
  return VkBufferMemoryBarrier2 {
    .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
    .srcStageMask        = source.stages,
    .srcAccessMask       = source.access,
    .dstStageMask        = destination.stages,
    .dstAccessMask       = destination.access,
    .srcQueueFamilyIndex = sourceQueueFamily,
    .dstQueueFamilyIndex = destinationQueueFamily,
    .buffer              = buffer,
    .offset              = offset,
    .size                = size,
  };
}

VkImageMemoryBarrier2 ImageBarrier(VkImage image, VkImageSubresourceRange range, ImageAccessScope source, ImageAccessScope destination)
{
  return VkImageMemoryBarrier2 {
    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
    .srcStageMask        = source.access.stages,
    .srcAccessMask       = source.access.access,
    .dstStageMask        = destination.access.stages,
    .dstAccessMask       = destination.access.access,
    .oldLayout           = source.layout,
    .newLayout           = destination.layout,
    .srcQueueFamilyIndex = source.queueFamily,
    .dstQueueFamilyIndex = destination.queueFamily,
    .image               = image,
    .subresourceRange    = range,
  };
}

void CmdBarriers(VkCommandBuffer commandBuffer, std::span<const VkMemoryBarrier2> memoryBarriers, std::span<const VkBufferMemoryBarrier2> bufferBarriers, std::span<const VkImageMemoryBarrier2> imageBarriers, VkDependencyFlags dependencyFlags)
{
  const VkDependencyInfo dependencyInfo {
    .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    .dependencyFlags          = dependencyFlags,
    .memoryBarrierCount       = static_cast<uint32_t>(memoryBarriers.size()),
    .pMemoryBarriers          = memoryBarriers.data(),
    .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
    .pBufferMemoryBarriers    = bufferBarriers.data(),
    .imageMemoryBarrierCount  = static_cast<uint32_t>(imageBarriers.size()),
    .pImageMemoryBarriers     = imageBarriers.data(),
  };

  vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
}

void CmdMemoryBarrier(VkCommandBuffer commandBuffer, AccessScope source, AccessScope destination)
{
  const std::array barrier { MemoryBarrier(source, destination) };

  CmdBarriers(commandBuffer, barrier);
}

void CmdBufferBarrier(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, AccessScope source, AccessScope destination)
{
  const std::array barrier { BufferBarrier(buffer, offset, size, source, destination) };

  CmdBarriers(commandBuffer, {}, barrier);
}

void CmdImageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageSubresourceRange range, ImageAccessScope source, ImageAccessScope destination)
{
  const std::array barrier { ImageBarrier(image, range, source, destination) };

  CmdBarriers(commandBuffer, {}, {}, barrier);
}

void TransitionStorageImageForWrite(VkCommandBuffer commandBuffer, Image& image, VkPipelineStageFlags2 destinationStages, StorageImageWriteOrdering ordering)
{
  // A null image has nothing to transition.
  if(!image)
  {
    return;
  }

  // First-transition-only callers leave an image the command buffer already moved to GENERAL alone.
  if(ordering == StorageImageWriteOrdering::eFirstTransitionOnly && image.descriptor.imageLayout == VK_IMAGE_LAYOUT_GENERAL)
  {
    return;
  }

  // Barrier
  // An image that already holds contents names ALL_COMMANDS as the earlier writer, which orders this access after any previous write; an UNDEFINED image has nothing to preserve, so it needs no source.
  // Storage images only ever sit in UNDEFINED or GENERAL, so first-transition-only callers always take the source-less form.

  const VkImageLayout oldLayout = image.descriptor.imageLayout;
  const bool hasPriorContents = oldLayout != VK_IMAGE_LAYOUT_UNDEFINED;

  const VkImageMemoryBarrier2 barrier {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask     = hasPriorContents ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask    = hasPriorContents ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_NONE,
      .dstStageMask     = destinationStages,
      .dstAccessMask    = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout        = oldLayout,
      .newLayout        = VK_IMAGE_LAYOUT_GENERAL,
      .image            = image.image,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
  };

  const VkDependencyInfo dependency {
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &barrier,
  };

  vkCmdPipelineBarrier2(commandBuffer, &dependency);

  // Recorded so later transitions and descriptor writes start from the layout the command buffer actually leaves the image in.
  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

}  // namespace rtpt
