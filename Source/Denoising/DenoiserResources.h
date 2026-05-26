#pragma once

#include <vulkan/vulkan_core.h>

#include "nvvk/resource_allocator.hpp"
#include "nvvk/resources.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

// Owns the NRD input images shared by ray-traced renderers:
// primary-hit guide buffers plus diffuse/specular split noisy signals.
class DenoiserResources
{
public:
  struct CreateInfo
  {
    nvapp::Application*      app       = nullptr;
    nvvk::ResourceAllocator* allocator = nullptr;
  };

  explicit DenoiserResources(const CreateInfo& createInfo);

  void Destroy();
  void EnsureForViewport(VkExtent2D viewportSize);

  VkExtent2D GetViewportSize() const;

  const nvvk::Image& GetMotionVectorsImage() const;
  nvvk::Image&       GetMotionVectorsImage();

  const nvvk::Image& GetNormalRoughnessImage() const;
  nvvk::Image&       GetNormalRoughnessImage();

  const nvvk::Image& GetBaseColorMetalnessImage() const;
  nvvk::Image&       GetBaseColorMetalnessImage();

  const nvvk::Image& GetViewZImage() const;
  nvvk::Image&       GetViewZImage();

  const nvvk::Image& GetDiffuseRadianceHitDistanceImage() const;
  nvvk::Image&       GetDiffuseRadianceHitDistanceImage();

  const nvvk::Image& GetSpecularRadianceHitDistanceImage() const;
  nvvk::Image&       GetSpecularRadianceHitDistanceImage();

  const nvvk::Image& GetSpecularDemodulationFactorImage() const;
  nvvk::Image&       GetSpecularDemodulationFactorImage();

private:
  void CreateOrResizeViewportResources(VkExtent2D viewportSize);
  void DestroyViewportResources();
  void ScheduleImageDestroy(nvvk::Image image);
  nvvk::Image CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const;

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  VkExtent2D               m_ViewportSize{};
  nvvk::Image              m_MotionVectorsImage;
  nvvk::Image              m_NormalRoughnessImage;
  nvvk::Image              m_BaseColorMetalnessImage;
  nvvk::Image              m_ViewZImage;
  nvvk::Image              m_DiffuseRadianceHitDistanceImage;
  nvvk::Image              m_SpecularRadianceHitDistanceImage;
  nvvk::Image              m_SpecularDemodulationFactorImage;
};

}  // namespace nvsamples
