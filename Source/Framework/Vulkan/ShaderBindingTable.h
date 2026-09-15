#pragma once

#include "GpuResources.h"

#include <span>

namespace rtpt
{

// ShaderBindingTableRegions
// The four address regions vkCmdTraceRaysKHR takes. A section with no groups is an all-zero region.

struct ShaderBindingTableRegions
{
  // Ray generation region; exactly one record.
  VkStridedDeviceAddressRegionKHR raygen {};
  // Miss shader records.
  VkStridedDeviceAddressRegionKHR miss {};
  // Hit group records.
  VkStridedDeviceAddressRegionKHR hit {};
  // Callable shader records.
  VkStridedDeviceAddressRegionKHR callable {};
};

// ShaderBindingTableGroups
// Which pipeline shader groups go into each section, as indices into the pipeline's group list, in record order.

struct ShaderBindingTableGroups
{
  // Must contain exactly one group.
  std::span<const uint32_t> raygen;
  // Miss groups.
  std::span<const uint32_t> miss;
  // Hit groups.
  std::span<const uint32_t> hit;
  // Callable groups.
  std::span<const uint32_t> callable;
};

// ShaderBindingTable
// Builds and owns the buffer of shader group handles a ray tracing pipeline is dispatched with.
// It owns the layout rules: record strides, section alignment, and the raygen region's size matching its stride.

class ShaderBindingTable
{
public:

  ShaderBindingTable() = default;
  ShaderBindingTable(const ShaderBindingTable&)            = delete;
  ShaderBindingTable& operator=(const ShaderBindingTable&) = delete;
  ~ShaderBindingTable();

  VkResult Initialize(ResourceAllocator& resources, VkPipeline pipeline, uint32_t pipelineGroupCount, const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties, const ShaderBindingTableGroups& groups);
  void Destroy();

  [[nodiscard]] const ShaderBindingTableRegions& Regions() const noexcept { return m_Regions; }
  [[nodiscard]] const Buffer& Storage() const noexcept { return m_Buffer; }

private:

  // Allocator that owns m_Buffer. Null until Initialize succeeds.
  ResourceAllocator*         m_Resources = nullptr;
  // Mapped buffer holding the handle records.
  Buffer                     m_Buffer {};
  // Regions into m_Buffer for each section.
  ShaderBindingTableRegions  m_Regions {};
};

}  // namespace rtpt
