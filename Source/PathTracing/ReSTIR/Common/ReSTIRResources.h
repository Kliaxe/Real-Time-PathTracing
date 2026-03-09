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

// Owns the GPU storage shared by the thesis ReSTIR methods. The reservoir
// payload stays method-specific, but the lifetime and ping-pong pattern are
// common infrastructure.
class ReSTIRResources
{
public:
  struct CreateInfo
  {
    nvapp::Application*      app       = nullptr;
    nvvk::ResourceAllocator* allocator = nullptr;
  };

  explicit ReSTIRResources(const CreateInfo& createInfo);

  void Destroy();
  // Ensures the per-pixel storage matches the active viewport.
  void EnsureForViewport(VkExtent2D viewportSize);

  VkExtent2D GetViewportSize() const;
  const nvvk::Buffer& GetInitialReservoirBuffer() const;
  const nvvk::Buffer& GetScratchReservoirBuffer() const;
  const nvvk::Buffer& GetHistoryReservoirBuffer(uint32_t historyIndex) const;
  const nvvk::Buffer& GetSurfaceBuffer(uint32_t historyIndex) const;
  const nvvk::Buffer& GetNeighborOffsetBuffer() const;
  const nvvk::Buffer& GetDebugBuffer() const;
  const nvvk::Image&  GetAccumulationImage() const;
  uint32_t            GetNeighborOffsetCount() const;

private:
  void CreateOrResizeViewportResources(VkExtent2D viewportSize);
  // Neighbor offsets are immutable support data for the spatial pass.
  void CreateNeighborOffsetBuffer();
  // Viewport-sized resources are retired asynchronously because the GPU may
  // still be reading the previous frame's buffers.
  void DestroyViewportResources();
  void ScheduleBufferDestroy(nvvk::Buffer buffer);
  void ScheduleImageDestroy(nvvk::Image image);
  nvvk::Buffer CreateStorageBuffer(VkDeviceSize size, const char* debugName) const;

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  VkExtent2D               m_ViewportSize{};
  // Two history slots let one frame read the previous state while writing the
  // current frame's state.
  std::array<nvvk::Buffer, 2> m_HistoryReservoirBuffers{};
  std::array<nvvk::Buffer, 2> m_SurfaceBuffers{};
  // Initial and scratch buffers are the transient hand-off points inside one
  // frame between the initial, temporal, and spatial passes.
  nvvk::Buffer                m_InitialReservoirBuffer;
  nvvk::Buffer                m_ScratchReservoirBuffer;
  // Support data shared by all ReSTIR methods.
  nvvk::Buffer                m_NeighborOffsetBuffer;
  nvvk::Buffer                m_DebugBuffer;
  // Final-image accumulation uses the same presentation path as the baseline
  // path tracer.
  nvvk::Image                 m_AccumulationImage;
  uint32_t                    m_NeighborOffsetCount = 0;
};

}  // namespace nvsamples
