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

class ReSTIRDIResources
{
public:
  struct CreateInfo
  {
    nvapp::Application*      app       = nullptr;
    nvvk::ResourceAllocator* allocator = nullptr;
  };

  explicit ReSTIRDIResources(const CreateInfo& createInfo);

  void Destroy();
  void EnsureForViewport(VkExtent2D viewportSize);

  VkExtent2D                          GetViewportSize() const;
  const nvvk::Buffer&                 GetLightReservoirBuffer() const;
  const nvvk::Buffer&                 GetSurfaceBuffer(uint32_t historyIndex) const;
  const nvvk::Buffer&                 GetNeighborOffsetBuffer() const;
  const nvvk::Buffer&                 GetDebugBuffer() const;
  const nvvk::Image&                  GetAccumulationImage() const;
  uint32_t                            GetNeighborOffsetCount() const;
  const ReSTIRReservoirBufferParameters& GetReservoirBufferParameters() const;

private:
  void CreateNeighborOffsetBuffer();
  void CreateOrResizeViewportResources(VkExtent2D viewportSize);
  void ScheduleBufferDestroy(nvvk::Buffer buffer);
  void ScheduleImageDestroy(nvvk::Image image);
  nvvk::Buffer CreateStorageBuffer(VkDeviceSize size, const char* debugName) const;

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  VkExtent2D               m_ViewportSize{};
  std::array<nvvk::Buffer, 2> m_SurfaceBuffers{};
  nvvk::Buffer             m_LightReservoirBuffer;
  nvvk::Buffer             m_NeighborOffsetBuffer;
  nvvk::Buffer             m_DebugBuffer;
  nvvk::Image              m_AccumulationImage;
  uint32_t                 m_NeighborOffsetCount = 0;
  ReSTIRReservoirBufferParameters m_ReservoirBufferParameters{};
};

}  // namespace nvsamples
