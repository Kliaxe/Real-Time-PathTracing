#include "UploadContext.h"

#include "Diagnostics.h"

#include <cstring>
#include <stdexcept>

namespace rtpt
{

void UploadContext::Initialize(ResourceAllocator& resources, GpuExecution& execution)
{
  m_Resources = &resources;
  m_Execution = &execution;
}

Buffer UploadContext::CreateStaging(std::span<const std::byte> bytes) const
{
  if(m_Resources == nullptr || bytes.empty())
  {
    throw std::invalid_argument("upload requires initialized services and non-empty data");
  }

  // A mapped buffer written once from start to end. The mapping may not be host-coherent, so the write is flushed before the GPU reads it.
  Buffer staging;
  CheckVk(m_Resources->CreateBuffer(staging, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT), "ResourceAllocator::CreateBuffer(upload staging)");

  std::memcpy(staging.mapping, bytes.data(), bytes.size());
  CheckVk(m_Resources->FlushBuffer(staging, 0, bytes.size()), "ResourceAllocator::FlushBuffer(upload staging)");

  return staging;
}

void UploadContext::UploadBuffer(VkBuffer destination, VkDeviceSize destinationOffset, std::span<const std::byte> bytes, AccessScope destinationAccess)
{
  // Copy and hand off
  // After the copy, a barrier moves the destination range from the copy's write access into the access the caller will use next.
  // The staging buffer is released on both the success and the exception path; its destruction is deferred past the completed submission.

  Buffer staging = CreateStaging(bytes);

  try
  {
    (void)m_Execution->ExecuteAndWait([&](VkCommandBuffer commandBuffer) {
      const VkBufferCopy copy { .srcOffset = 0, .dstOffset = destinationOffset, .size = bytes.size() };
      vkCmdCopyBuffer(commandBuffer, staging.buffer, destination, 1, &copy);

      CmdBufferBarrier(commandBuffer, destination, destinationOffset, bytes.size(), { .stages = VK_PIPELINE_STAGE_2_COPY_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT }, destinationAccess);
    });
  }
  catch(...)
  {
    m_Resources->DestroyBuffer(staging);
    throw;
  }

  m_Resources->DestroyBuffer(staging);
}

void UploadContext::UploadImage(const ImageUpload& destination, std::span<const std::byte> bytes)
{
  // Transition, copy, transition
  // The subresource is moved into TRANSFER_DST_OPTIMAL for the copy, then into the caller's final state. Only one mip level is affected.
  // Queue family ownership moves to the destination family in the first barrier and stays there, so the second barrier transfers nothing.
  // The staging buffer is released on both the success and the exception path.

  Buffer staging = CreateStaging(bytes);

  try
  {
    (void)m_Execution->ExecuteAndWait([&](VkCommandBuffer commandBuffer) {
      const VkImageSubresourceRange range {
        .aspectMask     = destination.subresource.aspectMask,
        .baseMipLevel   = destination.subresource.mipLevel,
        .levelCount     = 1,
        .baseArrayLayer = destination.subresource.baseArrayLayer,
        .layerCount     = destination.subresource.layerCount,
      };

      CmdImageBarrier(commandBuffer, destination.image, range, destination.before, { .access = { .stages = VK_PIPELINE_STAGE_2_COPY_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT }, .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .queueFamily = destination.after.queueFamily });

      const VkBufferImageCopy copy {
        .bufferOffset      = 0,
        .bufferRowLength   = static_cast<uint32_t>(destination.bufferRowLength),
        .bufferImageHeight = static_cast<uint32_t>(destination.bufferImageHeight),
        .imageSubresource  = destination.subresource,
        .imageExtent       = destination.extent,
      };

      vkCmdCopyBufferToImage(commandBuffer, staging.buffer, destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

      CmdImageBarrier(commandBuffer, destination.image, range, { .access = { .stages = VK_PIPELINE_STAGE_2_COPY_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT }, .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .queueFamily = destination.after.queueFamily }, destination.after);
    });
  }
  catch(...)
  {
    m_Resources->DestroyBuffer(staging);
    throw;
  }

  m_Resources->DestroyBuffer(staging);
}

}  // namespace rtpt
