#include "ReSTIRDIResources.h"

#include <array>
#include <cstdint>
#include <cstring>

#include <nvapp/application.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

#include "PathTracing/ReSTIR/ReSTIRDIParameterContext.h"

namespace nvsamples
{

namespace
{

constexpr VkFormat kAccumulationFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

std::array<shaderio::ReSTIRNeighborOffset, 16> CreateDefaultNeighborOffsets()
{
  // Fixed low-discrepancy pattern used by the spatial reuse pass. The shader
  // rotates the starting index per pixel so this table can stay small.
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

ReSTIRDIResources::ReSTIRDIResources(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
{
}

void ReSTIRDIResources::Destroy()
{
  // Destroy is used during full renderer shutdown where no submitted work should still reference these resources.
  for(nvvk::Buffer& surfaceBuffer : m_SurfaceBuffers)
  {
    m_Allocator->destroyBuffer(surfaceBuffer);
    surfaceBuffer = {};
  }

  m_Allocator->destroyBuffer(m_LightReservoirBuffer);
  m_LightReservoirBuffer = {};
  m_Allocator->destroyBuffer(m_NeighborOffsetBuffer);
  m_NeighborOffsetBuffer = {};
  m_Allocator->destroyBuffer(m_DebugBuffer);
  m_DebugBuffer = {};
  m_Allocator->destroyImage(m_AccumulationImage);
  m_AccumulationImage = {};

  m_ViewportSize = {};
  m_NeighborOffsetCount = 0;
  m_ReservoirBufferParameters = {};
}

void ReSTIRDIResources::EnsureForViewport(VkExtent2D viewportSize)
{
  if(m_NeighborOffsetBuffer.buffer == VK_NULL_HANDLE)
  {
    // Neighbor offsets do not depend on resolution, so create them once.
    CreateNeighborOffsetBuffer();
  }

  CreateOrResizeViewportResources(viewportSize);
}

VkExtent2D ReSTIRDIResources::GetViewportSize() const
{
  return m_ViewportSize;
}

const nvvk::Buffer& ReSTIRDIResources::GetLightReservoirBuffer() const
{
  return m_LightReservoirBuffer;
}

const nvvk::Buffer& ReSTIRDIResources::GetSurfaceBuffer(uint32_t historyIndex) const
{
  // Current/previous surfaces are ping-ponged by frame parity.
  return m_SurfaceBuffers[historyIndex & 1u];
}

const nvvk::Buffer& ReSTIRDIResources::GetNeighborOffsetBuffer() const
{
  return m_NeighborOffsetBuffer;
}

const nvvk::Buffer& ReSTIRDIResources::GetDebugBuffer() const
{
  return m_DebugBuffer;
}

const nvvk::Image& ReSTIRDIResources::GetAccumulationImage() const
{
  return m_AccumulationImage;
}

uint32_t ReSTIRDIResources::GetNeighborOffsetCount() const
{
  return m_NeighborOffsetCount;
}

const ReSTIRReservoirBufferParameters& ReSTIRDIResources::GetReservoirBufferParameters() const
{
  return m_ReservoirBufferParameters;
}

void ReSTIRDIResources::CreateNeighborOffsetBuffer()
{
  const auto offsets = CreateDefaultNeighborOffsets();
  m_NeighborOffsetCount = static_cast<uint32_t>(offsets.size());

  // Host-visible upload is fine here because the table is tiny and immutable.
  NVVK_CHECK(m_Allocator->createBuffer(
      m_NeighborOffsetBuffer, sizeof(shaderio::ReSTIRNeighborOffset) * offsets.size(), VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT));

  std::memcpy(m_NeighborOffsetBuffer.mapping, offsets.data(), sizeof(shaderio::ReSTIRNeighborOffset) * offsets.size());
  NVVK_CHECK(m_Allocator->flushBuffer(m_NeighborOffsetBuffer, 0, sizeof(shaderio::ReSTIRNeighborOffset) * offsets.size()));
  nvvk::DebugUtil::getInstance().setObjectName(m_NeighborOffsetBuffer.buffer, "ReSTIRDINeighborOffsetBuffer");
}

void ReSTIRDIResources::CreateOrResizeViewportResources(VkExtent2D viewportSize)
{
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height
     && m_LightReservoirBuffer.buffer != VK_NULL_HANDLE)
  {
    // Existing buffers already match the current resolution.
    return;
  }

  // The shader uses this layout to address packed reservoir arrays.
  m_ReservoirBufferParameters = CalculateReservoirBufferParameters(viewportSize.width, viewportSize.height);

  // One reservoir buffer contains all rotating reservoir arrays.
  const VkDeviceSize reservoirElementCount =
      VkDeviceSize(m_ReservoirBufferParameters.reservoirArrayPitch) * VkDeviceSize(kReSTIRDIReservoirBufferCount);
  const VkDeviceSize reservoirBufferSize = reservoirElementCount * sizeof(ReSTIRPackedDIReservoir);
  const VkDeviceSize pixelCount          = VkDeviceSize(viewportSize.width) * VkDeviceSize(viewportSize.height);
  const VkDeviceSize surfaceBufferSize   = pixelCount * sizeof(shaderio::ReSTIRDISurface);
  const VkDeviceSize debugBufferSize     = pixelCount * sizeof(shaderio::ReSTIRDebugPixel);

  // Old viewport resources may still be referenced by submitted frames, so defer destruction.
  for(nvvk::Buffer& surfaceBuffer : m_SurfaceBuffers)
  {
    ScheduleBufferDestroy(surfaceBuffer);
    surfaceBuffer = {};
  }
  ScheduleBufferDestroy(m_LightReservoirBuffer);
  ScheduleBufferDestroy(m_DebugBuffer);
  ScheduleImageDestroy(m_AccumulationImage);

  m_ViewportSize = viewportSize;
  m_SurfaceBuffers[0] = CreateStorageBuffer(surfaceBufferSize, "ReSTIRDISurfaceHistory0Buffer");
  m_SurfaceBuffers[1] = CreateStorageBuffer(surfaceBufferSize, "ReSTIRDISurfaceHistory1Buffer");

  m_LightReservoirBuffer = CreateStorageBuffer(reservoirBufferSize, "ReSTIRDILightReservoirBuffer");
  m_DebugBuffer          = CreateStorageBuffer(debugBufferSize, "ReSTIRDIDebugBuffer");

  // The accumulation image stores HDR radiance before tonemapping/denoising.
  VkImageCreateInfo imageInfo{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = kAccumulationFormat,
      .extent        = {.width = viewportSize.width, .height = viewportSize.height, .depth = 1},
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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
  nvvk::DebugUtil::getInstance().setObjectName(m_AccumulationImage.image, "ReSTIRDIAccumulationImage");
  nvvk::DebugUtil::getInstance().setObjectName(m_AccumulationImage.descriptor.imageView, "ReSTIRDIAccumulationImageView");
  m_AccumulationImage.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  m_AccumulationImage.descriptor.sampler     = VK_NULL_HANDLE;
}

void ReSTIRDIResources::ScheduleBufferDestroy(nvvk::Buffer buffer)
{
  if(buffer.buffer == VK_NULL_HANDLE)
  {
    return;
  }

  // nvpro runs this callback only when it is safe to free resources used by previous submissions.
  nvvk::ResourceAllocator* allocator = m_Allocator;
  m_App->submitResourceFree([allocator, buffer]() mutable {
    if(allocator != nullptr)
    {
      allocator->destroyBuffer(buffer);
    }
  });
}

void ReSTIRDIResources::ScheduleImageDestroy(nvvk::Image image)
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

nvvk::Buffer ReSTIRDIResources::CreateStorageBuffer(VkDeviceSize size, const char* debugName) const
{
  nvvk::Buffer buffer;
  // Device-local storage buffers are written by GPU passes and occasionally cleared by transfer.
  NVVK_CHECK(m_Allocator->createBuffer(buffer, size, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE));
  nvvk::DebugUtil::getInstance().setObjectName(buffer.buffer, debugName);
  return buffer;
}

}  // namespace nvsamples
