#include "SpatiotemporalBlueNoise.h"

#include <cstddef>
#include <span>
#include <stdexcept>

#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/UploadContext.h"
#include "Generated/Assets/BlueNoise64x64x32.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

SpatiotemporalBlueNoise::SpatiotemporalBlueNoise(const CreateInfo& createInfo)
    : m_Resources(createInfo.resources)
    , m_Uploads(createInfo.uploads)
    , m_Diagnostics(createInfo.diagnostics)
{
}

void SpatiotemporalBlueNoise::Initialize()
{
  // A second Initialize would leak the existing volume, so it is rejected along with missing services.
  if(m_Resources == nullptr || m_Uploads == nullptr || m_Volume)
  {
    throw std::runtime_error("spatiotemporal blue noise requires initialized upload services and single ownership");
  }

  // Dimensions
  // Width, height, and layer count come from the enum shared with shaders; the static_assert catches a regenerated volume that no longer matches them.

  constexpr uint32_t width  = shaderio::BlueNoiseDimensions::eBlueNoiseWidth;
  constexpr uint32_t height = shaderio::BlueNoiseDimensions::eBlueNoiseHeight;
  constexpr uint32_t layers = shaderio::BlueNoiseDimensions::eBlueNoiseLayers;

  static_assert(sizeof(BlueNoise64x64x32) == width * height * layers);

  // Volume image
  // One R8_UINT layer per frame of the sequence. Shaders load integer ranks with Load rather than filtering them, so no sampler is involved.

  const VkImageCreateInfo imageInfo {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = VK_FORMAT_R8_UINT,
      .extent        = { width, height, 1 },
      .mipLevels     = 1,
      .arrayLayers   = layers,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  const VkImageViewCreateInfo viewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format           = imageInfo.format,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = layers },
  };

  CheckVk(m_Resources->CreateImage(m_Volume, imageInfo, &viewInfo), "ResourceAllocator::CreateImage(spatiotemporal blue noise)");

  // Upload
  // Every layer is uploaded in one call and left readable by ray-tracing shaders, with compute access for sampling validation. A failed upload destroys the image, leaving the object uninitialized.

  try
  {
    const auto bytes = std::as_bytes(std::span(BlueNoise64x64x32));

    m_Uploads->UploadImage({ .image = m_Volume.image, .extent = imageInfo.extent, .subresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = layers }, .after = { .access = { .stages = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT }, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } }, bytes);

    m_Volume.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  catch(...)
  {
    Destroy();
    throw;
  }

  // Debug names are optional.
  if(m_Diagnostics != nullptr)
  {
    m_Diagnostics->SetObjectName(m_Resources->Device(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(m_Volume.image), "Spatiotemporal Blue Noise");
    m_Diagnostics->SetObjectName(m_Resources->Device(), VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(m_Volume.descriptor.imageView), "Spatiotemporal Blue Noise View");
  }
}

void SpatiotemporalBlueNoise::Destroy()
{
  m_Volume.Reset();
}

const VkDescriptorImageInfo& SpatiotemporalBlueNoise::Descriptor() const noexcept
{
  return m_Volume.descriptor;
}

}  // namespace rtpt
