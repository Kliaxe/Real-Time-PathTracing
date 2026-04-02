#include "ReSTIRSharedResources.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/vec2.hpp>
#include <nvapp/application.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

#include "ReSTIR/Common/ReSTIRUtils.h"

namespace nvsamples
{

namespace
{

constexpr VkFormat kAccumulationFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr uint32_t kRtxdiNeighborOffsetCount = 8192;

std::array<shaderio::ReSTIRNeighborOffset, 16> CreateDefaultNeighborOffsets()
{
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

ReSTIRSharedResources::ReSTIRSharedResources(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_NeighborOffsetMode(createInfo.neighborOffsetMode)
{
}

void ReSTIRSharedResources::Destroy()
{
  for(nvvk::Buffer& surfaceBuffer : m_SurfaceBuffers)
  {
    m_Allocator->destroyBuffer(surfaceBuffer);
    surfaceBuffer = {};
  }

  m_Allocator->destroyBuffer(m_NeighborOffsetBuffer);
  m_NeighborOffsetBuffer = {};
  m_NeighborOffsetCount  = 0;

  m_Allocator->destroyImage(m_AccumulationImage);
  m_AccumulationImage = {};
  m_ViewportSize      = {};
}

void ReSTIRSharedResources::EnsureForViewport(VkExtent2D viewportSize, VkDeviceSize surfaceBufferSize)
{
  if(m_NeighborOffsetBuffer.buffer == VK_NULL_HANDLE)
  {
    CreateNeighborOffsetBuffer();
  }

  CreateOrResizeViewportResources(viewportSize, surfaceBufferSize);
}

VkExtent2D ReSTIRSharedResources::GetViewportSize() const
{
  return m_ViewportSize;
}

const nvvk::Buffer& ReSTIRSharedResources::GetSurfaceBuffer(uint32_t historyIndex) const
{
  return m_SurfaceBuffers[historyIndex & 1u];
}

const nvvk::Buffer& ReSTIRSharedResources::GetNeighborOffsetBuffer() const
{
  return m_NeighborOffsetBuffer;
}

const nvvk::Image& ReSTIRSharedResources::GetAccumulationImage() const
{
  return m_AccumulationImage;
}

nvvk::Image& ReSTIRSharedResources::GetAccumulationImage()
{
  return m_AccumulationImage;
}

uint32_t ReSTIRSharedResources::GetNeighborOffsetCount() const
{
  return m_NeighborOffsetCount;
}

void ReSTIRSharedResources::CreateNeighborOffsetBuffer()
{
  if(m_NeighborOffsetMode == ReSTIRNeighborOffsetMode::eThesisPattern)
  {
    const auto offsets = CreateDefaultNeighborOffsets();
    m_NeighborOffsetCount = static_cast<uint32_t>(offsets.size());

    NVVK_CHECK(m_Allocator->createBuffer(
        m_NeighborOffsetBuffer, sizeof(shaderio::ReSTIRNeighborOffset) * offsets.size(), VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT));

    std::memcpy(m_NeighborOffsetBuffer.mapping, offsets.data(), sizeof(shaderio::ReSTIRNeighborOffset) * offsets.size());
    NVVK_CHECK(m_Allocator->flushBuffer(m_NeighborOffsetBuffer, 0, sizeof(shaderio::ReSTIRNeighborOffset) * offsets.size()));
    NVVK_DBG_NAME(m_NeighborOffsetBuffer.buffer);
    return;
  }

  m_NeighborOffsetCount = kRtxdiNeighborOffsetCount;
  NVVK_CHECK(m_Allocator->createBuffer(
      m_NeighborOffsetBuffer, sizeof(glm::vec2) * m_NeighborOffsetCount, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT));

  std::vector<uint8_t> packedOffsets(m_NeighborOffsetCount * 2u);
  restir::FillNeighborOffsetBuffer(packedOffsets.data(), m_NeighborOffsetCount);

  std::vector<glm::vec2> normalizedOffsets(m_NeighborOffsetCount, glm::vec2(0.0f));
  for(uint32_t i = 0; i < m_NeighborOffsetCount; ++i)
  {
    const int8_t ox = static_cast<int8_t>(packedOffsets[i * 2u + 0u]);
    const int8_t oy = static_cast<int8_t>(packedOffsets[i * 2u + 1u]);
    normalizedOffsets[i] = glm::vec2(std::clamp(float(ox) / 127.0f, -1.0f, 1.0f), std::clamp(float(oy) / 127.0f, -1.0f, 1.0f));
  }

  std::memcpy(m_NeighborOffsetBuffer.mapping, normalizedOffsets.data(), sizeof(glm::vec2) * normalizedOffsets.size());
  NVVK_CHECK(m_Allocator->flushBuffer(m_NeighborOffsetBuffer, 0, sizeof(glm::vec2) * normalizedOffsets.size()));
  NVVK_DBG_NAME(m_NeighborOffsetBuffer.buffer);
}

void ReSTIRSharedResources::CreateOrResizeViewportResources(VkExtent2D viewportSize, VkDeviceSize surfaceBufferSize)
{
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height
     && m_SurfaceBuffers[0].buffer != VK_NULL_HANDLE)
  {
    return;
  }

  DestroyViewportResources();
  m_ViewportSize = viewportSize;

  for(nvvk::Buffer& surfaceBuffer : m_SurfaceBuffers)
  {
    surfaceBuffer = CreateStorageBuffer(surfaceBufferSize, "ReSTIRSharedSurfaceBuffer");
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
  m_AccumulationImage.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  m_AccumulationImage.descriptor.sampler     = VK_NULL_HANDLE;
}

void ReSTIRSharedResources::DestroyViewportResources()
{
  for(nvvk::Buffer& surfaceBuffer : m_SurfaceBuffers)
  {
    ScheduleBufferDestroy(surfaceBuffer);
    surfaceBuffer = {};
  }

  ScheduleImageDestroy(m_AccumulationImage);
  m_AccumulationImage = {};
}

void ReSTIRSharedResources::ScheduleBufferDestroy(nvvk::Buffer buffer)
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

void ReSTIRSharedResources::ScheduleImageDestroy(nvvk::Image image)
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

nvvk::Buffer ReSTIRSharedResources::CreateStorageBuffer(VkDeviceSize size, const char* debugName) const
{
  (void)debugName;
  nvvk::Buffer buffer;
  NVVK_CHECK(
      m_Allocator->createBuffer(buffer, size, VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE));
  NVVK_DBG_NAME(buffer.buffer);
  return buffer;
}

}  // namespace nvsamples
