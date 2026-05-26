#include "DenoiserResources.h"

#include <nvapp/application.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

namespace nvsamples
{

namespace
{

constexpr VkFormat kMotionVectorsFormat    = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kNormalRoughnessFormat  = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kBaseColorMetalnessFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kViewZFormat            = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kRadianceHitDistFormat  = VK_FORMAT_R16G16B16A16_SFLOAT;

}  // namespace

DenoiserResources::DenoiserResources(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
{
}

void DenoiserResources::Destroy()
{
  m_Allocator->destroyImage(m_MotionVectorsImage);
  m_Allocator->destroyImage(m_NormalRoughnessImage);
  m_Allocator->destroyImage(m_BaseColorMetalnessImage);
  m_Allocator->destroyImage(m_ViewZImage);
  m_Allocator->destroyImage(m_DiffuseRadianceHitDistanceImage);
  m_Allocator->destroyImage(m_SpecularRadianceHitDistanceImage);

  m_MotionVectorsImage               = {};
  m_NormalRoughnessImage             = {};
  m_BaseColorMetalnessImage          = {};
  m_ViewZImage                       = {};
  m_DiffuseRadianceHitDistanceImage  = {};
  m_SpecularRadianceHitDistanceImage = {};
  m_ViewportSize                     = {};
}

void DenoiserResources::EnsureForViewport(VkExtent2D viewportSize)
{
  CreateOrResizeViewportResources(viewportSize);
}

VkExtent2D DenoiserResources::GetViewportSize() const
{
  return m_ViewportSize;
}

const nvvk::Image& DenoiserResources::GetMotionVectorsImage() const
{
  return m_MotionVectorsImage;
}

nvvk::Image& DenoiserResources::GetMotionVectorsImage()
{
  return m_MotionVectorsImage;
}

const nvvk::Image& DenoiserResources::GetNormalRoughnessImage() const
{
  return m_NormalRoughnessImage;
}

nvvk::Image& DenoiserResources::GetNormalRoughnessImage()
{
  return m_NormalRoughnessImage;
}

const nvvk::Image& DenoiserResources::GetBaseColorMetalnessImage() const
{
  return m_BaseColorMetalnessImage;
}

nvvk::Image& DenoiserResources::GetBaseColorMetalnessImage()
{
  return m_BaseColorMetalnessImage;
}

const nvvk::Image& DenoiserResources::GetViewZImage() const
{
  return m_ViewZImage;
}

nvvk::Image& DenoiserResources::GetViewZImage()
{
  return m_ViewZImage;
}

const nvvk::Image& DenoiserResources::GetDiffuseRadianceHitDistanceImage() const
{
  return m_DiffuseRadianceHitDistanceImage;
}

nvvk::Image& DenoiserResources::GetDiffuseRadianceHitDistanceImage()
{
  return m_DiffuseRadianceHitDistanceImage;
}

const nvvk::Image& DenoiserResources::GetSpecularRadianceHitDistanceImage() const
{
  return m_SpecularRadianceHitDistanceImage;
}

nvvk::Image& DenoiserResources::GetSpecularRadianceHitDistanceImage()
{
  return m_SpecularRadianceHitDistanceImage;
}

void DenoiserResources::CreateOrResizeViewportResources(VkExtent2D viewportSize)
{
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height
     && m_MotionVectorsImage.image != VK_NULL_HANDLE)
  {
    return;
  }

  DestroyViewportResources();
  m_ViewportSize = viewportSize;

  // These images are the renderer-side half of the NRD contract. Path tracing
  // and ReSTIR both write the same guide buffers plus noisy diffuse/specular
  // radiance-hit-distance signals before NRD consumes them.
  m_MotionVectorsImage   = CreateStorageImage(viewportSize, kMotionVectorsFormat, "PathTraceMotionVectorsImage");
  m_NormalRoughnessImage = CreateStorageImage(viewportSize, kNormalRoughnessFormat, "PathTraceNormalRoughnessImage");
  m_BaseColorMetalnessImage = CreateStorageImage(viewportSize, kBaseColorMetalnessFormat, "PathTraceBaseColorMetalnessImage");
  m_ViewZImage           = CreateStorageImage(viewportSize, kViewZFormat, "PathTraceViewZImage");
  m_DiffuseRadianceHitDistanceImage =
      CreateStorageImage(viewportSize, kRadianceHitDistFormat, "PathTraceDiffuseRadianceHitDistanceImage");
  m_SpecularRadianceHitDistanceImage =
      CreateStorageImage(viewportSize, kRadianceHitDistFormat, "PathTraceSpecularRadianceHitDistanceImage");
}

void DenoiserResources::DestroyViewportResources()
{
  ScheduleImageDestroy(m_MotionVectorsImage);
  ScheduleImageDestroy(m_NormalRoughnessImage);
  ScheduleImageDestroy(m_BaseColorMetalnessImage);
  ScheduleImageDestroy(m_ViewZImage);
  ScheduleImageDestroy(m_DiffuseRadianceHitDistanceImage);
  ScheduleImageDestroy(m_SpecularRadianceHitDistanceImage);

  m_MotionVectorsImage               = {};
  m_NormalRoughnessImage             = {};
  m_BaseColorMetalnessImage          = {};
  m_ViewZImage                       = {};
  m_DiffuseRadianceHitDistanceImage  = {};
  m_SpecularRadianceHitDistanceImage = {};
}

void DenoiserResources::ScheduleImageDestroy(nvvk::Image image)
{
  if(image.image == VK_NULL_HANDLE)
  {
    return;
  }

  nvvk::ResourceAllocator* allocator = m_Allocator;
  m_App->submitResourceFree([allocator, image]() mutable {
    if(allocator != nullptr)
    {
      allocator->destroyImage(image);
    }
  });
}

nvvk::Image DenoiserResources::CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const
{
  nvvk::Image image;

  VkImageCreateInfo imageInfo{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = format,
      .extent        = {.width = viewportSize.width, .height = viewportSize.height, .depth = 1},
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  VkImageViewCreateInfo viewInfo{
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = imageInfo.format,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
  };

  NVVK_CHECK(m_Allocator->createImage(image, imageInfo, viewInfo));
  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image.descriptor.sampler     = VK_NULL_HANDLE;
  (void)debugName;
  NVVK_DBG_NAME(image.image);
  NVVK_DBG_NAME(image.descriptor.imageView);
  return image;
}

}  // namespace nvsamples
