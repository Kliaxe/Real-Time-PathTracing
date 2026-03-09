#include "ReSTIRResources.h"

#include <array>
#include <cstring>
#include <span>

#include <nvapp/application.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

namespace nvsamples
{

namespace
{

constexpr VkFormat kAccumulationFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

std::array<shaderio::ReSTIRNeighborOffset, 16> CreateDefaultNeighborOffsets()
{
  // A fixed low-discrepancy pattern keeps spatial sampling deterministic across
  // methods so differences come from the estimator, not from offset noise.
  return {{
      {{-0.94201624f, -0.39906216f}, {0.0f, 0.0f}},
      {{0.94558609f, -0.76890725f}, {0.0f, 0.0f}},
      {{-0.09418410f, -0.92938870f}, {0.0f, 0.0f}},
      {{0.34495938f, 0.29387760f}, {0.0f, 0.0f}},
      {{-0.91588581f, 0.45771432f}, {0.0f, 0.0f}},
      {{-0.81544232f, -0.87912464f}, {0.0f, 0.0f}},
      {{-0.38277543f, 0.27676845f}, {0.0f, 0.0f}},
      {{0.97484398f, 0.75648379f}, {0.0f, 0.0f}},
      {{0.44323325f, -0.97511554f}, {0.0f, 0.0f}},
      {{0.53742981f, -0.47373420f}, {0.0f, 0.0f}},
      {{-0.26496911f, -0.41893023f}, {0.0f, 0.0f}},
      {{0.79197514f, 0.19090188f}, {0.0f, 0.0f}},
      {{-0.24188840f, 0.99706507f}, {0.0f, 0.0f}},
      {{-0.81409955f, 0.91437590f}, {0.0f, 0.0f}},
      {{0.19984126f, 0.78641367f}, {0.0f, 0.0f}},
      {{0.14383161f, -0.14100790f}, {0.0f, 0.0f}},
  }};
}

}  // namespace

ReSTIRResources::ReSTIRResources(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
{
}

void ReSTIRResources::Destroy()
{
  // Full shutdown can destroy immediately because the app waits for the queue
  // before calling this.
  m_Allocator->destroyBuffer(m_InitialReservoirBuffer);
  m_Allocator->destroyBuffer(m_ScratchReservoirBuffer);
  m_Allocator->destroyBuffer(m_DebugBuffer);
  m_InitialReservoirBuffer = {};
  m_ScratchReservoirBuffer = {};
  m_DebugBuffer            = {};

  for(uint32_t i = 0; i < 2; ++i)
  {
    m_Allocator->destroyBuffer(m_HistoryReservoirBuffers[i]);
    m_Allocator->destroyBuffer(m_SurfaceBuffers[i]);
    m_HistoryReservoirBuffers[i] = {};
    m_SurfaceBuffers[i]          = {};
  }

  m_Allocator->destroyImage(m_AccumulationImage);
  m_AccumulationImage = {};
  m_Allocator->destroyBuffer(m_NeighborOffsetBuffer);
  m_NeighborOffsetBuffer  = {};
  m_NeighborOffsetCount   = 0;
  m_ViewportSize          = {};
}

void ReSTIRResources::EnsureForViewport(VkExtent2D viewportSize)
{
  if(m_NeighborOffsetBuffer.buffer == VK_NULL_HANDLE)
  {
    CreateNeighborOffsetBuffer();
  }

  CreateOrResizeViewportResources(viewportSize);
}

VkExtent2D ReSTIRResources::GetViewportSize() const
{
  return m_ViewportSize;
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
  return m_SurfaceBuffers[historyIndex & 1u];
}

const nvvk::Buffer& ReSTIRResources::GetNeighborOffsetBuffer() const
{
  return m_NeighborOffsetBuffer;
}

const nvvk::Buffer& ReSTIRResources::GetDebugBuffer() const
{
  return m_DebugBuffer;
}

const nvvk::Image& ReSTIRResources::GetAccumulationImage() const
{
  return m_AccumulationImage;
}

uint32_t ReSTIRResources::GetNeighborOffsetCount() const
{
  return m_NeighborOffsetCount;
}

void ReSTIRResources::CreateOrResizeViewportResources(VkExtent2D viewportSize)
{
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height
     && m_InitialReservoirBuffer.buffer != VK_NULL_HANDLE)
  {
    // Reuse the current allocation when only per-frame contents change.
    return;
  }

  DestroyViewportResources();
  m_ViewportSize = viewportSize;

  const VkDeviceSize pixelCount = VkDeviceSize(viewportSize.width) * VkDeviceSize(viewportSize.height);
  // Every pass indexes these buffers directly by pixel index, so the storage is
  // tightly packed with no extra indirection.
  const VkDeviceSize reservoirBufferSize = pixelCount * sizeof(shaderio::ReSTIRPTReservoir);
  const VkDeviceSize surfaceBufferSize   = pixelCount * sizeof(shaderio::ReSTIRPTPrimarySurface);
  const VkDeviceSize debugBufferSize     = pixelCount * sizeof(shaderio::ReSTIRDebugPixel);

  m_InitialReservoirBuffer = CreateStorageBuffer(reservoirBufferSize, "ReSTIRPTInitialReservoirBuffer");
  m_ScratchReservoirBuffer = CreateStorageBuffer(reservoirBufferSize, "ReSTIRPTScratchReservoirBuffer");
  m_DebugBuffer            = CreateStorageBuffer(debugBufferSize, "ReSTIRPTDebugBuffer");
  for(uint32_t i = 0; i < 2; ++i)
  {
    // History and surface buffers share the same ping-pong index so temporal
    // lookups always read matching receiver and reservoir state.
    m_HistoryReservoirBuffers[i] = CreateStorageBuffer(reservoirBufferSize, "ReSTIRPTHistoryReservoirBuffer");
    m_SurfaceBuffers[i]          = CreateStorageBuffer(surfaceBufferSize, "ReSTIRPTSurfaceBuffer");
  }

  VkImageCreateInfo imageInfo{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = kAccumulationFormat,
      .extent        = {.width = viewportSize.width, .height = viewportSize.height, .depth = 1},
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_STORAGE_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VkImageViewCreateInfo viewInfo{
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = imageInfo.format,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
  };

  NVVK_CHECK(m_Allocator->createImage(m_AccumulationImage, imageInfo, viewInfo));
  // The renderer records the first layout transition lazily before the image is
  // used as storage.
  m_AccumulationImage.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  m_AccumulationImage.descriptor.sampler     = VK_NULL_HANDLE;
}

void ReSTIRResources::CreateNeighborOffsetBuffer()
{
  const auto offsets = CreateDefaultNeighborOffsets();
  m_NeighborOffsetCount = static_cast<uint32_t>(offsets.size());

  NVVK_CHECK(m_Allocator->createBuffer(
      m_NeighborOffsetBuffer, sizeof(shaderio::ReSTIRNeighborOffset) * offsets.size(),
      VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT));

  // Host-visible initialization is sufficient because this buffer is small and
  // immutable after creation.
  std::memcpy(m_NeighborOffsetBuffer.mapping, offsets.data(), sizeof(shaderio::ReSTIRNeighborOffset) * offsets.size());
  NVVK_DBG_NAME(m_NeighborOffsetBuffer.buffer);
}

void ReSTIRResources::DestroyViewportResources()
{
  // Resize paths defer destruction through the app free queue so the previous
  // frame can finish using the old storage safely.
  ScheduleBufferDestroy(m_InitialReservoirBuffer);
  ScheduleBufferDestroy(m_ScratchReservoirBuffer);
  ScheduleBufferDestroy(m_DebugBuffer);
  m_InitialReservoirBuffer = {};
  m_ScratchReservoirBuffer = {};
  m_DebugBuffer            = {};

  for(uint32_t i = 0; i < 2; ++i)
  {
    ScheduleBufferDestroy(m_HistoryReservoirBuffers[i]);
    ScheduleBufferDestroy(m_SurfaceBuffers[i]);
    m_HistoryReservoirBuffers[i] = {};
    m_SurfaceBuffers[i]          = {};
  }

  ScheduleImageDestroy(m_AccumulationImage);
  m_AccumulationImage = {};
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

void ReSTIRResources::ScheduleImageDestroy(nvvk::Image image)
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

nvvk::Buffer ReSTIRResources::CreateStorageBuffer(VkDeviceSize size, const char* debugName) const
{
  (void)debugName;
  nvvk::Buffer buffer;
  // Reservoir and surface buffers stay device-local because the renderer never
  // needs to map them after creation.
  NVVK_CHECK(m_Allocator->createBuffer(buffer, size, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE));
  NVVK_DBG_NAME(buffer.buffer);
  return buffer;
}

}  // namespace nvsamples
