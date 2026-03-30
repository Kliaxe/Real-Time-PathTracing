#include "ReSTIRDIResources.h"

#include <nvapp/application.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

#include "ReSTIR/DI/ReSTIRDI.h"

namespace nvsamples
{

namespace
{

constexpr restir::CheckerboardMode kCheckerboardMode = restir::CheckerboardMode::Off;

}  // namespace

ReSTIRDIResources::ReSTIRDIResources(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_SharedResources(ReSTIRSharedResources::CreateInfo{
          .app = createInfo.app, .allocator = createInfo.allocator, .neighborOffsetMode = ReSTIRNeighborOffsetMode::eRTXDINormalizedFloat2})
{
}

void ReSTIRDIResources::Destroy()
{
  m_Allocator->destroyBuffer(m_LightReservoirBuffer);
  m_LightReservoirBuffer = {};

  m_SharedResources.Destroy();
  m_ReservoirBufferParameters = {};
}

void ReSTIRDIResources::EnsureForViewport(VkExtent2D viewportSize)
{
  CreateOrResizeViewportResources(viewportSize);
}

VkExtent2D ReSTIRDIResources::GetViewportSize() const
{
  return m_SharedResources.GetViewportSize();
}

const nvvk::Buffer& ReSTIRDIResources::GetLightReservoirBuffer() const
{
  return m_LightReservoirBuffer;
}

const nvvk::Buffer& ReSTIRDIResources::GetSurfaceBuffer(uint32_t historyIndex) const
{
  return m_SharedResources.GetSurfaceBuffer(historyIndex);
}

const nvvk::Buffer& ReSTIRDIResources::GetNeighborOffsetBuffer() const
{
  return m_SharedResources.GetNeighborOffsetBuffer();
}

const nvvk::Image& ReSTIRDIResources::GetAccumulationImage() const
{
  return m_SharedResources.GetAccumulationImage();
}

uint32_t ReSTIRDIResources::GetNeighborOffsetCount() const
{
  return m_SharedResources.GetNeighborOffsetCount();
}

const ReSTIRReservoirBufferParameters& ReSTIRDIResources::GetReservoirBufferParameters() const
{
  return m_ReservoirBufferParameters;
}

void ReSTIRDIResources::CreateOrResizeViewportResources(VkExtent2D viewportSize)
{
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  if(m_SharedResources.GetViewportSize().width == viewportSize.width && m_SharedResources.GetViewportSize().height == viewportSize.height
     && m_LightReservoirBuffer.buffer != VK_NULL_HANDLE)
  {
    return;
  }

  m_ReservoirBufferParameters =
      restir::CalculateReservoirBufferParameters(viewportSize.width, viewportSize.height, kCheckerboardMode);

  const VkDeviceSize reservoirElementCount =
      VkDeviceSize(m_ReservoirBufferParameters.reservoirArrayPitch) * VkDeviceSize(restir::c_NumReSTIRDIReservoirBuffers);
  const VkDeviceSize reservoirBufferSize = reservoirElementCount * sizeof(ReSTIRPackedDIReservoir);
  const VkDeviceSize pixelCount          = VkDeviceSize(viewportSize.width) * VkDeviceSize(viewportSize.height);
  const VkDeviceSize surfaceBufferSize   = pixelCount * sizeof(shaderio::ReSTIRDISurface);

  m_SharedResources.EnsureForViewport(viewportSize, surfaceBufferSize);
  ScheduleBufferDestroy(m_LightReservoirBuffer);
  m_LightReservoirBuffer = CreateStorageBuffer(reservoirBufferSize, "ReSTIRDILightReservoirBuffer");
}

void ReSTIRDIResources::ScheduleBufferDestroy(nvvk::Buffer buffer)
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

nvvk::Buffer ReSTIRDIResources::CreateStorageBuffer(VkDeviceSize size, const char* debugName) const
{
  (void)debugName;
  nvvk::Buffer buffer;
  NVVK_CHECK(m_Allocator->createBuffer(buffer, size, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE));
  NVVK_DBG_NAME(buffer.buffer);
  return buffer;
}

}  // namespace nvsamples
