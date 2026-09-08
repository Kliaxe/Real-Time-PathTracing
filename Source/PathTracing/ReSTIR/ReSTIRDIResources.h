#pragma once

#include <array>

#include <vulkan/vulkan_core.h>

#include "ReSTIR/Parameters.h"
#include "Shaders/ShaderIo.h"
#include "nvvk/resource_allocator.hpp"
#include "nvvk/resources.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

// Owns the ReSTIR DI buffers/images whose size depends on the viewport.
// This class does not decide what a pass does. It only guarantees that the
// GPU memory used by those passes exists and matches the current resolution.
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
  // Creates missing resources and recreates viewport-sized resources when resolution changes.
  void EnsureForViewport(VkExtent2D viewportSize);

  VkExtent2D                             GetViewportSize() const;
  // Three logical reservoir arrays live inside this one packed buffer.
  const nvvk::Buffer&                    GetLightReservoirBuffer() const;
  // Two surface buffers ping-pong between current and previous frame history.
  const nvvk::Buffer&                    GetSurfaceBuffer(uint32_t historyIndex) const;
  const nvvk::Buffer&                    GetNeighborOffsetBuffer() const;
  const nvvk::Buffer&                    GetDebugBuffer() const;
  const nvvk::Image&                     GetAccumulationImage() const;
  uint32_t                               GetNeighborOffsetCount() const;
  const ReSTIRReservoirBufferParameters& GetReservoirBufferParameters() const;

private:
  // Neighbor offsets are static for the app lifetime and reused every frame.
  void CreateNeighborOffsetBuffer();
  // Viewport resources are replaced instead of resized in place.
  void CreateOrResizeViewportResources(VkExtent2D viewportSize);
  // Old resources are freed after the GPU has finished using submitted work.
  void ScheduleBufferDestroy(nvvk::Buffer buffer);
  void ScheduleImageDestroy(nvvk::Image image);
  nvvk::Buffer CreateStorageBuffer(VkDeviceSize size, const char* debugName) const;

  nvapp::Application*             m_App       = nullptr;
  nvvk::ResourceAllocator*        m_Allocator = nullptr;
  VkExtent2D                      m_ViewportSize{};
  // Indexed by ReSTIRFrameContext current/previous history index.
  std::array<nvvk::Buffer, 2>     m_SurfaceBuffers{};
  nvvk::Buffer                    m_LightReservoirBuffer;
  nvvk::Buffer                    m_NeighborOffsetBuffer;
  nvvk::Buffer                    m_DebugBuffer;
  nvvk::Image                     m_AccumulationImage;
  uint32_t                        m_NeighborOffsetCount = 0;
  ReSTIRReservoirBufferParameters m_ReservoirBufferParameters{};
};

}  // namespace nvsamples
