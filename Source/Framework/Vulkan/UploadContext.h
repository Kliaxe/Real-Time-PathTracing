#pragma once

#include "Barriers.h"
#include "GpuExecution.h"
#include "GpuResources.h"

#include <span>

namespace rtpt
{

// ImageUpload
// Destination of an image upload: which subresource receives the bytes and the access states around the copy.

struct ImageUpload
{
  // Destination image.
  VkImage                   image = VK_NULL_HANDLE;
  // Texel extent of the copied region.
  VkExtent3D                extent {};
  // Mip level and layers written. Exactly one mip level is transitioned.
  VkImageSubresourceLayers  subresource {};
  // Row length of the source data in texels. Zero means it matches the extent.
  uint32_t                  bufferRowLength = 0;
  // Image height of the source data in texels. Zero means it matches the extent.
  uint32_t                  bufferImageHeight = 0;
  // Image state before the upload.
  ImageAccessScope          before {};
  // Image state left after the upload. Its queue family also owns the image during the copy.
  ImageAccessScope          after {};
};

// UploadContext
// Copies host bytes into device buffers and images through a staging buffer and a blocking one-shot submission.
// Because it waits for the GPU, it is meant for load-time data and cannot run while a frame is open.

class UploadContext
{
public:

  void Initialize(ResourceAllocator& resources, GpuExecution& execution);

  // Leaves the destination range in destinationAccess.
  void UploadBuffer(VkBuffer destination, VkDeviceSize destinationOffset, std::span<const std::byte> bytes, AccessScope destinationAccess);
  void UploadImage(const ImageUpload& destination, std::span<const std::byte> bytes);

private:

  // Creates a mapped staging buffer already holding the bytes.
  [[nodiscard]] Buffer CreateStaging(std::span<const std::byte> bytes) const;

  // Allocates staging buffers.
  ResourceAllocator* m_Resources = nullptr;
  // Runs the copy submissions.
  GpuExecution*      m_Execution = nullptr;
};

}  // namespace rtpt
