#include "ReSTIRResources.h"

#include <nvapp/application.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

namespace nvsamples
{

ReSTIRResources::ReSTIRResources(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_SharedResources(ReSTIRSharedResources::CreateInfo{
          .app = createInfo.app, .allocator = createInfo.allocator, .neighborOffsetMode = ReSTIRNeighborOffsetMode::eThesisPattern})
{
}

void ReSTIRResources::Destroy()
{
  m_Allocator->destroyBuffer(m_InitialReservoirBuffer);
  m_Allocator->destroyBuffer(m_ScratchReservoirBuffer);
  m_Allocator->destroyBuffer(m_DebugBuffer);
  m_InitialReservoirBuffer = {};
  m_ScratchReservoirBuffer = {};
  m_DebugBuffer            = {};

  for(uint32_t i = 0; i < 2; ++i)
  {
    m_Allocator->destroyBuffer(m_HistoryReservoirBuffers[i]);
    m_HistoryReservoirBuffers[i] = {};
  }

  m_SharedResources.Destroy();
}

void ReSTIRResources::EnsureForViewport(VkExtent2D viewportSize)
{
  CreateOrResizeViewportResources(viewportSize);
}

VkExtent2D ReSTIRResources::GetViewportSize() const
{
  return m_SharedResources.GetViewportSize();
}

const nvvk::Buffer& ReSTIRResources::GetInitialReservoirBuffer() const
{
  return m_InitialReservoirBuffer;
}

const nvvk::Buffer& ReSTIRResources::GetScratchReservoirBuffer() const
{
  return m_ScratchReservoirBuffer;
}

const nvvk::Buffer& ReSTIRResources::GetHistoryReservoirBuffer(uint32_t historyIndex) const
{
  return m_HistoryReservoirBuffers[historyIndex & 1u];
}

const nvvk::Buffer& ReSTIRResources::GetSurfaceBuffer(uint32_t historyIndex) const
{
  return m_SharedResources.GetSurfaceBuffer(historyIndex);
}

const nvvk::Buffer& ReSTIRResources::GetNeighborOffsetBuffer() const
{
  return m_SharedResources.GetNeighborOffsetBuffer();
}

const nvvk::Buffer& ReSTIRResources::GetDebugBuffer() const
{
  return m_DebugBuffer;
}

const nvvk::Image& ReSTIRResources::GetAccumulationImage() const
{
  return m_SharedResources.GetAccumulationImage();
}

uint32_t ReSTIRResources::GetNeighborOffsetCount() const
{
  return m_SharedResources.GetNeighborOffsetCount();
}

void ReSTIRResources::CreateOrResizeViewportResources(VkExtent2D viewportSize)
{
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  if(m_SharedResources.GetViewportSize().width == viewportSize.width && m_SharedResources.GetViewportSize().height == viewportSize.height
     && m_InitialReservoirBuffer.buffer != VK_NULL_HANDLE)
  {
    return;
  }

  const VkDeviceSize pixelCount          = VkDeviceSize(viewportSize.width) * VkDeviceSize(viewportSize.height);
  const VkDeviceSize reservoirBufferSize = pixelCount * sizeof(shaderio::ReSTIRPTReservoir);
  const VkDeviceSize surfaceBufferSize   = pixelCount * sizeof(shaderio::ReSTIRPTPrimarySurface);
  const VkDeviceSize debugBufferSize     = pixelCount * sizeof(shaderio::ReSTIRDebugPixel);

  m_SharedResources.EnsureForViewport(viewportSize, surfaceBufferSize);

  ScheduleBufferDestroy(m_InitialReservoirBuffer);
  ScheduleBufferDestroy(m_ScratchReservoirBuffer);
  ScheduleBufferDestroy(m_DebugBuffer);
  m_InitialReservoirBuffer = {};
  m_ScratchReservoirBuffer = {};
  m_DebugBuffer            = {};

  for(uint32_t i = 0; i < 2; ++i)
  {
    ScheduleBufferDestroy(m_HistoryReservoirBuffers[i]);
    m_HistoryReservoirBuffers[i] = {};
  }

  m_InitialReservoirBuffer = CreateStorageBuffer(reservoirBufferSize, "ReSTIRPTInitialReservoirBuffer");
  m_ScratchReservoirBuffer = CreateStorageBuffer(reservoirBufferSize, "ReSTIRPTScratchReservoirBuffer");
  m_DebugBuffer            = CreateStorageBuffer(debugBufferSize, "ReSTIRPTDebugBuffer");
  for(uint32_t i = 0; i < 2; ++i)
  {
    m_HistoryReservoirBuffers[i] = CreateStorageBuffer(reservoirBufferSize, "ReSTIRPTHistoryReservoirBuffer");
  }
}

void ReSTIRResources::ScheduleBufferDestroy(nvvk::Buffer buffer)
{
  if(buffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  nvvk::ResourceAllocator* allocator = m_Allocator;
  m_App->submitResourceFree([allocator, buffer]() mutable {
    if(allocator != nullptr)
    {
      allocator->destroyBuffer(buffer);
    }
  });
}

nvvk::Buffer ReSTIRResources::CreateStorageBuffer(VkDeviceSize size, const char* debugName) const
{
  (void)debugName;
  nvvk::Buffer buffer;
  NVVK_CHECK(m_Allocator->createBuffer(buffer, size, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE));
  NVVK_DBG_NAME(buffer.buffer);
  return buffer;
}

}  // namespace nvsamples
