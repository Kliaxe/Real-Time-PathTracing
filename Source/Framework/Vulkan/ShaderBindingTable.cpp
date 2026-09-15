#include "ShaderBindingTable.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace rtpt
{
namespace
{

// Rounds up to a multiple of a power-of-two alignment.
constexpr VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

ShaderBindingTable::~ShaderBindingTable()
{
  Destroy();
}

VkResult ShaderBindingTable::Initialize(ResourceAllocator& resources, VkPipeline pipeline, uint32_t pipelineGroupCount, const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties, const ShaderBindingTableGroups& groups)
{
  // Validation
  // A trace dispatch uses exactly one ray generation shader, and every listed group must exist in the pipeline.

  if(m_Resources != nullptr || pipeline == VK_NULL_HANDLE || pipelineGroupCount == 0 || groups.raygen.size() != 1 || properties.shaderGroupHandleSize == 0 || properties.shaderGroupHandleAlignment == 0 || properties.shaderGroupBaseAlignment == 0)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const std::array groupLists { groups.raygen, groups.miss, groups.hit, groups.callable };
  for(const std::span<const uint32_t> list : groupLists)
  {
    if(std::ranges::any_of(list, [pipelineGroupCount](uint32_t index) { return index >= pipelineGroupCount; }))
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  // Layout
  // A record is one handle padded to the handle alignment and may not exceed the device's maximum stride.
  // Vulkan requires the raygen region's size to equal its stride, and a region's start to be a multiple of the base alignment. So the raygen stride is widened to the base alignment and every section is padded to it.
  // Sections are laid out raygen, miss, hit, callable.

  const VkDeviceSize handleSize = properties.shaderGroupHandleSize;
  const VkDeviceSize recordStride = AlignUp(handleSize, properties.shaderGroupHandleAlignment);

  if(recordStride > properties.maxShaderGroupStride)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  std::array<VkDeviceSize, 4> strides { AlignUp(recordStride, properties.shaderGroupBaseAlignment), recordStride, recordStride, recordStride };
  std::array<VkDeviceSize, 4> offsets {};
  VkDeviceSize bufferSize = 0;

  for(size_t section = 0; section < groupLists.size(); ++section)
  {
    offsets[section] = bufferSize;
    bufferSize += AlignUp(strides[section] * groupLists[section].size(), properties.shaderGroupBaseAlignment);
  }

  // The whole table is memset and copied through a host mapping, so its size must fit in size_t.
  if(bufferSize == 0 || bufferSize > std::numeric_limits<size_t>::max())
  {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  // Storage
  // A mapped, sequentially written buffer aligned to the base alignment, so offsets that are multiples of it are valid region starts.

  Buffer buffer;
  VkResult result = resources.CreateBuffer(buffer, bufferSize, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, properties.shaderGroupBaseAlignment);

  if(result != VK_SUCCESS)
  {
    return result;
  }

  // Handles
  // All group handles are fetched at once, then each record copies the handle of the group it names. Padding is zeroed.
  // The mapping may not be host-coherent, so the writes are flushed.

  std::vector<std::byte> handles(static_cast<size_t>(pipelineGroupCount) * properties.shaderGroupHandleSize);
  result = vkGetRayTracingShaderGroupHandlesKHR(resources.Device(), pipeline, 0, pipelineGroupCount, handles.size(), handles.data());

  if(result != VK_SUCCESS)
  {
    resources.DestroyBuffer(buffer);
    return result;
  }

  std::memset(buffer.mapping, 0, static_cast<size_t>(bufferSize));

  for(size_t section = 0; section < groupLists.size(); ++section)
  {
    for(size_t record = 0; record < groupLists[section].size(); ++record)
    {
      const uint32_t groupIndex = groupLists[section][record];
      std::memcpy(buffer.mapping + offsets[section] + record * strides[section], handles.data() + static_cast<size_t>(groupIndex) * properties.shaderGroupHandleSize, properties.shaderGroupHandleSize);
    }
  }

  result = resources.FlushBuffer(buffer);

  if(result != VK_SUCCESS)
  {
    resources.DestroyBuffer(buffer);
    return result;
  }

  // Regions
  // An empty section becomes an all-zero region, which trace dispatch accepts for unused sections.

  const VkDeviceAddress bufferAddress = buffer.address;

  auto makeRegion = [bufferAddress, &offsets, &strides, &groupLists](size_t section) {
    if(groupLists[section].empty())
    {
      return VkStridedDeviceAddressRegionKHR {};
    }
    return VkStridedDeviceAddressRegionKHR {
      .deviceAddress = bufferAddress + offsets[section],
      .stride        = strides[section],
      .size          = strides[section] * groupLists[section].size(),
    };
  };

  m_Regions = {
    .raygen   = makeRegion(0),
    .miss     = makeRegion(1),
    .hit      = makeRegion(2),
    .callable = makeRegion(3),
  };

  m_Resources = &resources;
  m_Buffer    = std::move(buffer);

  return VK_SUCCESS;
}

void ShaderBindingTable::Destroy()
{
  if(m_Resources != nullptr)
  {
    m_Resources->DestroyBuffer(m_Buffer);
  }

  m_Resources = nullptr;
  m_Buffer    = {};
  m_Regions   = {};
}

}  // namespace rtpt
