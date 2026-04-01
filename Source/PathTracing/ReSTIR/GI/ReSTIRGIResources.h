#pragma once

#include <vulkan/vulkan_core.h>

#include "PathTracing/ReSTIR/Common/ReSTIRSharedResources.h"
#include "Shaders/ShaderIo.h"
#include "nvvk/resource_allocator.hpp"
#include "nvvk/resources.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

class ReSTIRGIResources
{
public:
  struct CreateInfo
  {
    nvapp::Application*      app       = nullptr;
    nvvk::ResourceAllocator* allocator = nullptr;
  };

  explicit ReSTIRGIResources(const CreateInfo& createInfo);

  void Destroy();
  void EnsureForViewport(VkExtent2D viewportSize);

  VkExtent2D                             GetViewportSize() const;
  const nvvk::Buffer&                    GetReservoirBuffer() const;
  const nvvk::Buffer&                    GetInitialSampleBuffer() const;
  const nvvk::Buffer&                    GetSurfaceBuffer(uint32_t historyIndex) const;
  const nvvk::Buffer&                    GetNeighborOffsetBuffer() const;
  const nvvk::Image&                     GetAccumulationImage() const;
  uint32_t                               GetNeighborOffsetCount() const;
  const ReSTIRReservoirBufferParameters& GetReservoirBufferParameters() const;

private:
  void CreateOrResizeViewportResources(VkExtent2D viewportSize);
  void ScheduleBufferDestroy(nvvk::Buffer buffer);
  nvvk::Buffer CreateStorageBuffer(VkDeviceSize size, const char* debugName) const;

  nvapp::Application*             m_App       = nullptr;
  nvvk::ResourceAllocator*        m_Allocator = nullptr;
  ReSTIRSharedResources           m_SharedResources;
  nvvk::Buffer                    m_ReservoirBuffer;
  nvvk::Buffer                    m_InitialSampleBuffer;
  ReSTIRReservoirBufferParameters m_ReservoirBufferParameters{};
};

}  // namespace nvsamples
