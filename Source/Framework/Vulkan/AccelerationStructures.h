#pragma once

#include "GpuExecution.h"
#include "GpuResources.h"

#include <span>
#include <vector>

namespace rtpt
{

// AccelerationStructureBuild
// Describes one bottom- or top-level acceleration structure build: its geometry, build ranges, and the sizes Vulkan reports for them.
// Sizing (Finalize) is split from recording (Record) so AccelerationStructureBuilder can allocate every destination and one shared scratch buffer before any command is recorded.
// Geometry is frozen once finalized because the reported sizes are only valid for the geometry they were computed from.

class AccelerationStructureBuild
{
public:

  explicit AccelerationStructureBuild(VkAccelerationStructureTypeKHR type, VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);

  // Throws if the build is already finalized.
  void AddGeometry(const VkAccelerationStructureGeometryKHR& geometry, const VkAccelerationStructureBuildRangeInfoKHR& range);

  // Queries build sizes. Fails if there is no geometry, the build is already finalized, or the driver reports a zero size.
  VkResult Finalize(VkDevice device);

  [[nodiscard]] VkAccelerationStructureCreateInfoKHR CreateInfo() const;
  [[nodiscard]] VkDeviceSize ScratchSize() const noexcept { return m_Sizes.buildScratchSize; }
  [[nodiscard]] bool Ready() const noexcept { return m_Finalized; }

  // Records the build into an existing destination using scratch memory at the given device address.
  void Record(VkCommandBuffer commandBuffer, VkAccelerationStructureKHR destination, VkDeviceAddress scratchAddress) const;

private:

  // Bottom- or top-level.
  VkAccelerationStructureTypeKHR                     m_Type;
  // Build preference flags, fast trace by default.
  VkBuildAccelerationStructureFlagsKHR               m_Flags;
  // Geometry descriptions passed to Vulkan by pointer; any buffers they reference must stay alive until the build executes.
  std::vector<VkAccelerationStructureGeometryKHR>    m_Geometries;
  // One build range per geometry, index-aligned with m_Geometries.
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> m_Ranges;
  // Destination and scratch sizes reported by Finalize.
  VkAccelerationStructureBuildSizesInfoKHR           m_Sizes { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
  // Set once sizes are known; gates allocation and recording.
  bool                                               m_Finalized = false;
};

// AccelerationStructureBuilder
// Turns finalized builds into live acceleration structures with a single blocking submission.
// It owns the policy of sharing one scratch buffer across a batch and rolling back every destination if any step fails.

class AccelerationStructureBuilder
{
public:

  // The scratch alignment must be a non-zero power of two, normally minAccelerationStructureScratchOffsetAlignment.
  void Initialize(ResourceAllocator& resources, GpuExecution& execution, VkDeviceSize scratchAlignment);

  // Builds are recorded in order, so a top-level build can follow the bottom-level builds it references. Destinations must be empty.
  VkResult Build(std::span<const AccelerationStructureBuild> builds, std::span<AccelerationStructure> accelerationStructures) const;
  VkResult Build(const AccelerationStructureBuild& build, AccelerationStructure& accelerationStructure) const;

private:

  // Allocates destinations and the scratch buffer.
  ResourceAllocator* m_Resources = nullptr;
  // Runs the one-shot build submission and waits for it.
  GpuExecution*      m_Execution = nullptr;
  // Minimum alignment for the scratch buffer, from the device's acceleration-structure properties.
  VkDeviceSize       m_ScratchAlignment = 0;
};

// Converts a column-major 4x4 matrix, such as a glm::mat4, into Vulkan's row-major 3x4 instance transform. The bottom row is dropped.
VkTransformMatrixKHR ToTransformMatrix(const float* columnMajorMatrix4x4);

}  // namespace rtpt
