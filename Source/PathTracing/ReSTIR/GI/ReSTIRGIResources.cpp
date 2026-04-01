#include "ReSTIRGIResources.h"

#include <nvapp/application.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

#include "ReSTIR/GI/ReSTIRGI.h"

namespace nvsamples
{

namespace
{

constexpr restir::CheckerboardMode kCheckerboardMode = restir::CheckerboardMode::Off;

}

ReSTIRGIResources::ReSTIRGIResources(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_SharedResources(ReSTIRSharedResources::CreateInfo{
          .app = createInfo.app, .allocator = createInfo.allocator, .neighborOffsetMode = ReSTIRNeighborOffsetMode::eRTXDINormalizedFloat2})
{
}

void ReSTIRGIResources::Destroy()
{
  m_Allocator->destroyBuffer(m_ReservoirBuffer);
  m_ReservoirBuffer = {};
  m_Allocator->destroyBuffer(m_InitialSampleBuffer);
  m_InitialSampleBuffer = {};

  m_SharedResources.Destroy();
  m_ReservoirBufferParameters = {};
}

void ReSTIRGIResources::EnsureForViewport(VkExtent2D viewportSize)
{
  CreateOrResizeViewportResources(viewportSize);
}

VkExtent2D ReSTIRGIResources::GetViewportSize() const
{
  return m_SharedResources.GetViewportSize();
}

const nvvk::Buffer& ReSTIRGIResources::GetReservoirBuffer() const
{
  return m_ReservoirBuffer;
}

const nvvk::Buffer& ReSTIRGIResources::GetInitialSampleBuffer() const
{
  return m_InitialSampleBuffer;
}

const nvvk::Buffer& ReSTIRGIResources::GetSurfaceBuffer(uint32_t historyIndex) const
{
  return m_SharedResources.GetSurfaceBuffer(historyIndex);
}

const nvvk::Buffer& ReSTIRGIResources::GetNeighborOffsetBuffer() const
{
  return m_SharedResources.GetNeighborOffsetBuffer();
}

const nvvk::Image& ReSTIRGIResources::GetAccumulationImage() const
{
  return m_SharedResources.GetAccumulationImage();
}

uint32_t ReSTIRGIResources::GetNeighborOffsetCount() const
{
  return m_SharedResources.GetNeighborOffsetCount();
}

const ReSTIRReservoirBufferParameters& ReSTIRGIResources::GetReservoirBufferParameters() const
{
  return m_ReservoirBufferParameters;
}

void ReSTIRGIResources::CreateOrResizeViewportResources(VkExtent2D viewportSize)
{
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  if(m_SharedResources.GetViewportSize().width == viewportSize.width && m_SharedResources.GetViewportSize().height == viewportSize.height
     && m_ReservoirBuffer.buffer != VK_NULL_HANDLE)
  {
    return;
  }

  m_ReservoirBufferParameters =
      restir::CalculateReservoirBufferParameters(viewportSize.width, viewportSize.height, kCheckerboardMode);

  const VkDeviceSize reservoirElementCount =
      VkDeviceSize(m_ReservoirBufferParameters.reservoirArrayPitch) * VkDeviceSize(restir::c_NumReSTIRGIReservoirBuffers);
  const VkDeviceSize reservoirBufferSize = reservoirElementCount * sizeof(shaderio::ReSTIRGIReservoir);
  const VkDeviceSize pixelCount          = VkDeviceSize(viewportSize.width) * VkDeviceSize(viewportSize.height);
  const VkDeviceSize initialSampleBufferSize = pixelCount * sizeof(shaderio::ReSTIRGIReservoir);
  const VkDeviceSize surfaceBufferSize   = pixelCount * sizeof(shaderio::ReSTIRGIPrimarySurface);

  m_SharedResources.EnsureForViewport(viewportSize, surfaceBufferSize);
  ScheduleBufferDestroy(m_ReservoirBuffer);
  ScheduleBufferDestroy(m_InitialSampleBuffer);
  m_ReservoirBuffer = CreateStorageBuffer(reservoirBufferSize, "ReSTIRGIReservoirBuffer");
  m_InitialSampleBuffer = CreateStorageBuffer(initialSampleBufferSize, "ReSTIRGIInitialSampleBuffer");
}

void ReSTIRGIResources::ScheduleBufferDestroy(nvvk::Buffer buffer)
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

nvvk::Buffer ReSTIRGIResources::CreateStorageBuffer(VkDeviceSize size, const char* debugName) const
{
  (void)debugName;
  nvvk::Buffer buffer;
  NVVK_CHECK(m_Allocator->createBuffer(buffer, size, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE));
  NVVK_DBG_NAME(buffer.buffer);
  return buffer;
}

}  // namespace nvsamples
