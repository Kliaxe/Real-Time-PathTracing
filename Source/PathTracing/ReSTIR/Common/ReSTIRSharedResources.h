#pragma once

#include <array>

#include <vulkan/vulkan_core.h>

#include "Shaders/ShaderIo.h"
#include "nvvk/resource_allocator.hpp"
#include "nvvk/resources.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

enum class ReSTIRNeighborOffsetMode : uint32_t
{
  eThesisPattern = 0,
  eRTXDINormalizedFloat2,
};

class ReSTIRSharedResources
{
public:
  struct CreateInfo
  {
    nvapp::Application*      app                = nullptr;
    nvvk::ResourceAllocator* allocator          = nullptr;
    ReSTIRNeighborOffsetMode neighborOffsetMode = ReSTIRNeighborOffsetMode::eThesisPattern;
  };

  explicit ReSTIRSharedResources(const CreateInfo& createInfo);

  void Destroy();
  void EnsureForViewport(VkExtent2D viewportSize, VkDeviceSize surfaceBufferSize);

  VkExtent2D          GetViewportSize() const;
  const nvvk::Buffer& GetSurfaceBuffer(uint32_t historyIndex) const;
  const nvvk::Buffer& GetNeighborOffsetBuffer() const;
  const nvvk::Image&  GetAccumulationImage() const;
  nvvk::Image&        GetAccumulationImage();
  uint32_t            GetNeighborOffsetCount() const;

private:
  void CreateNeighborOffsetBuffer();
  void CreateOrResizeViewportResources(VkExtent2D viewportSize, VkDeviceSize surfaceBufferSize);
  void DestroyViewportResources();
  void ScheduleBufferDestroy(nvvk::Buffer buffer);
  void ScheduleImageDestroy(nvvk::Image image);
  nvvk::Buffer CreateStorageBuffer(VkDeviceSize size, const char* debugName) const;

  nvapp::Application*         m_App       = nullptr;
  nvvk::ResourceAllocator*    m_Allocator = nullptr;
  ReSTIRNeighborOffsetMode    m_NeighborOffsetMode = ReSTIRNeighborOffsetMode::eThesisPattern;
  VkExtent2D                  m_ViewportSize{};
  std::array<nvvk::Buffer, 2> m_SurfaceBuffers{};
  nvvk::Buffer                m_NeighborOffsetBuffer;
  nvvk::Image                 m_AccumulationImage;
  uint32_t                    m_NeighborOffsetCount = 0;
};

}  // namespace nvsamples
