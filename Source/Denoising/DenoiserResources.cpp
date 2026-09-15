#include "DenoiserResources.h"

#include <stdexcept>
#include <utility>

namespace rtpt
{

namespace
{

// Image formats
// Half float is enough for the guides and the packed radiance signals.
// ViewZ is full float because the shaders write a 1e32 "no surface" sentinel, which is far outside the half-float range.

constexpr VkFormat kMotionVectorsFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kNormalRoughnessFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kBaseColorMetalnessFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kViewZFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kRadianceHitDistFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kSpecularDemodulationFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
}

DenoiserResources::DenoiserResources(const CreateInfo& createInfo)
    : m_Resources(createInfo.resources)
    , m_Diagnostics(createInfo.diagnostics)
{
  // Every other method allocates through the allocator, so fail at construction rather than on first use.
  if(m_Resources == nullptr)
  {
    throw std::invalid_argument("DenoiserResources requires a resource allocator");
  }
}

void DenoiserResources::Destroy()
{
  DestroyViewportResources();

  // Clearing the size forces the next EnsureForViewport to allocate again.
  m_ViewportSize = {};
}

void DenoiserResources::EnsureForViewport(VkExtent2D viewportSize)
{
  CreateOrResizeViewportResources(viewportSize);
}

VkExtent2D DenoiserResources::GetViewportSize() const
{
  return m_ViewportSize;
}

const rtpt::Image& DenoiserResources::GetMotionVectorsImage() const
{
  return m_MotionVectorsImage;
}

rtpt::Image& DenoiserResources::GetMotionVectorsImage()
{
  return m_MotionVectorsImage;
}

const rtpt::Image& DenoiserResources::GetNormalRoughnessImage() const
{
  return m_NormalRoughnessImage;
}

rtpt::Image& DenoiserResources::GetNormalRoughnessImage()
{
  return m_NormalRoughnessImage;
}

const rtpt::Image& DenoiserResources::GetBaseColorMetalnessImage() const
{
  return m_BaseColorMetalnessImage;
}

rtpt::Image& DenoiserResources::GetBaseColorMetalnessImage()
{
  return m_BaseColorMetalnessImage;
}

const rtpt::Image& DenoiserResources::GetViewZImage() const
{
  return m_ViewZImage;
}

rtpt::Image& DenoiserResources::GetViewZImage()
{
  return m_ViewZImage;
}

const rtpt::Image& DenoiserResources::GetDiffuseRadianceHitDistanceImage() const
{
  return m_DiffuseRadianceHitDistanceImage;
}

rtpt::Image& DenoiserResources::GetDiffuseRadianceHitDistanceImage()
{
  return m_DiffuseRadianceHitDistanceImage;
}

const rtpt::Image& DenoiserResources::GetSpecularRadianceHitDistanceImage() const
{
  return m_SpecularRadianceHitDistanceImage;
}

rtpt::Image& DenoiserResources::GetSpecularRadianceHitDistanceImage()
{
  return m_SpecularRadianceHitDistanceImage;
}

const rtpt::Image& DenoiserResources::GetSpecularDemodulationFactorImage() const
{
  return m_SpecularDemodulationFactorImage;
}

rtpt::Image& DenoiserResources::GetSpecularDemodulationFactorImage()
{
  return m_SpecularDemodulationFactorImage;
}

void DenoiserResources::CreateOrResizeViewportResources(VkExtent2D viewportSize)
{
  // A zero extent cannot back an image, so the current images are kept. Application never renders while minimized and never resizes the viewport to zero, so this is only a defensive guard.
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  // Reallocating at an unchanged size would only discard the renderer's written signals.
  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height && m_MotionVectorsImage)
  {
    return;
  }

  // Allocate the new set
  // Every image is created before any member is replaced. CreateStorageImage throws on failure, so a failed allocation leaves the previous set intact.

  rtpt::Image motion = CreateStorageImage(viewportSize, kMotionVectorsFormat, "PathTraceMotionVectorsImage");
  rtpt::Image normal = CreateStorageImage(viewportSize, kNormalRoughnessFormat, "PathTraceNormalRoughnessImage");
  rtpt::Image baseColor = CreateStorageImage(viewportSize, kBaseColorMetalnessFormat, "PathTraceBaseColorMetalnessImage");
  rtpt::Image viewZ = CreateStorageImage(viewportSize, kViewZFormat, "PathTraceViewZImage");
  rtpt::Image diffuse = CreateStorageImage(viewportSize, kRadianceHitDistFormat, "PathTraceDiffuseRadianceHitDistanceImage");
  rtpt::Image specular = CreateStorageImage(viewportSize, kRadianceHitDistFormat, "PathTraceSpecularRadianceHitDistanceImage");
  rtpt::Image demodulation = CreateStorageImage(viewportSize, kSpecularDemodulationFormat, "PathTraceSpecularDemodulationFactorImage");

  // Swap in
  // Move-assignment releases each old image as its replacement lands.

  m_MotionVectorsImage = std::move(motion);
  m_NormalRoughnessImage = std::move(normal);
  m_BaseColorMetalnessImage = std::move(baseColor);
  m_ViewZImage = std::move(viewZ);
  m_DiffuseRadianceHitDistanceImage = std::move(diffuse);
  m_SpecularRadianceHitDistanceImage = std::move(specular);
  m_SpecularDemodulationFactorImage = std::move(demodulation);
  m_ViewportSize = viewportSize;
}

void DenoiserResources::DestroyViewportResources()
{
  m_MotionVectorsImage.Reset();
  m_NormalRoughnessImage.Reset();
  m_BaseColorMetalnessImage.Reset();
  m_ViewZImage.Reset();
  m_DiffuseRadianceHitDistanceImage.Reset();
  m_SpecularRadianceHitDistanceImage.Reset();
  m_SpecularDemodulationFactorImage.Reset();
}

rtpt::Image DenoiserResources::CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const
{
  // Image description
  // Storage for the renderer's shader writes, sampled for NRD and the compose pass reads.

  const VkImageCreateInfo imageInfo {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = { viewportSize.width, viewportSize.height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  const VkImageViewCreateInfo viewInfo {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
  };

  // Allocate
  // descriptor.imageLayout is the CPU-side record of the image's layout that the transition helpers read, so it starts as UNDEFINED.

  rtpt::Image image;

  rtpt::CheckVk(m_Resources->CreateImage(image, imageInfo, &viewInfo), "ResourceAllocator::CreateImage(denoiser target)");

  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if(m_Diagnostics != nullptr)
  {
    m_Diagnostics->SetObjectName(m_Resources->Device(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image.image), debugName);
  }

  return image;
}

}  // namespace rtpt
