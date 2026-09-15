#pragma once

#include <volk.h>

#include "Framework/Vulkan/GpuResources.h"

namespace rtpt
{

class Diagnostics;
class ResourceAllocator;
class UploadContext;

// SpatiotemporalBlueNoise
// Owns the GPU copy of the embedded 64x64x32 blue-noise volume as a 2D texture array.
// Both tracers and ReSTIR replay read ranks directly for addressable sample dimensions; the sampling code controls spatial translations and temporal epochs.

class SpatiotemporalBlueNoise
{
public:

  // CreateInfo
  // Services the volume borrows. Resources and uploads are required by Initialize; diagnostics is optional.

  struct CreateInfo
  {
    // Allocator for the volume image.
    ResourceAllocator* resources = nullptr;

    // Uploads the embedded bytes.
    UploadContext* uploads = nullptr;

    // Names the image for debugging tools when present.
    const Diagnostics* diagnostics = nullptr;
  };

  explicit SpatiotemporalBlueNoise(const CreateInfo& createInfo);

  void Initialize();

  void Destroy();

  [[nodiscard]] const VkDescriptorImageInfo& Descriptor() const noexcept;

private:

  // Allocator for the volume image.
  ResourceAllocator* m_Resources = nullptr;

  // Uploads the embedded bytes.
  UploadContext* m_Uploads = nullptr;

  // Names the image for debugging tools; may be null.
  const Diagnostics* m_Diagnostics = nullptr;

  // R8_UINT texture array, one layer per frame of the noise sequence.
  Image m_Volume;
};

}  // namespace rtpt
