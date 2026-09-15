#include "AccelerationStructures.h"

#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace rtpt
{

AccelerationStructureBuild::AccelerationStructureBuild(VkAccelerationStructureTypeKHR type, VkBuildAccelerationStructureFlagsKHR flags)
  : m_Type(type), m_Flags(flags)
{
}

void AccelerationStructureBuild::AddGeometry(const VkAccelerationStructureGeometryKHR& geometry, const VkAccelerationStructureBuildRangeInfoKHR& range)
{
  // Sizes computed by Finalize would no longer describe the geometry.
  if(m_Finalized)
  {
    throw std::logic_error("acceleration-structure geometry cannot change after finalization");
  }

  m_Geometries.push_back(geometry);
  m_Ranges.push_back(range);
}

VkResult AccelerationStructureBuild::Finalize(VkDevice device)
{
  if(device == VK_NULL_HANDLE || m_Finalized || m_Geometries.empty())
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  // Size query
  // Vulkan sizes a build from the geometry descriptions plus one maximum primitive count per geometry, so the counts are pulled out of the ranges.

  const std::vector<uint32_t> primitiveCounts = [&] {
    std::vector<uint32_t> counts;
    counts.reserve(m_Ranges.size());
    for(const VkAccelerationStructureBuildRangeInfoKHR& range : m_Ranges)
    {
      counts.push_back(range.primitiveCount);
    }
    return counts;
  }();

  const VkAccelerationStructureBuildGeometryInfoKHR buildInfo {
    .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
    .type          = m_Type,
    .flags         = m_Flags,
    .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
    .geometryCount = static_cast<uint32_t>(m_Geometries.size()),
    .pGeometries   = m_Geometries.data(),
  };

  vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, primitiveCounts.data(), &m_Sizes);

  // A zero size cannot be allocated, so the build is treated as unusable.
  if(m_Sizes.accelerationStructureSize == 0 || m_Sizes.buildScratchSize == 0)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  m_Finalized = true;

  return VK_SUCCESS;
}

VkAccelerationStructureCreateInfoKHR AccelerationStructureBuild::CreateInfo() const
{
  if(!m_Finalized)
  {
    throw std::logic_error("acceleration-structure build must be finalized before allocation");
  }

  // The allocator fills in the storage buffer.
  return VkAccelerationStructureCreateInfoKHR {
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
    .size  = m_Sizes.accelerationStructureSize,
    .type  = m_Type,
  };
}

void AccelerationStructureBuild::Record(VkCommandBuffer commandBuffer, VkAccelerationStructureKHR destination, VkDeviceAddress scratchAddress) const
{
  if(!m_Finalized || commandBuffer == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || scratchAddress == 0)
  {
    throw std::logic_error("invalid acceleration-structure build recording");
  }

  const VkAccelerationStructureBuildGeometryInfoKHR buildInfo {
    .sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
    .type                     = m_Type,
    .flags                    = m_Flags,
    .mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
    .dstAccelerationStructure = destination,
    .geometryCount            = static_cast<uint32_t>(m_Geometries.size()),
    .pGeometries              = m_Geometries.data(),
    .scratchData              = { .deviceAddress = scratchAddress },
  };

  // The build command takes one pointer to a range array per geometry, rather than the range array itself.
  std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
  ranges.reserve(m_Ranges.size());
  for(const VkAccelerationStructureBuildRangeInfoKHR& range : m_Ranges)
  {
    ranges.push_back(&range);
  }

  vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, ranges.data());
}

void AccelerationStructureBuilder::Initialize(ResourceAllocator& resources, GpuExecution& execution, VkDeviceSize scratchAlignment)
{
  // The alignment is passed to VMA, which requires a power of two.
  if(m_Resources != nullptr || resources.Handle() == VK_NULL_HANDLE || scratchAlignment == 0 || (scratchAlignment & (scratchAlignment - 1)) != 0)
  {
    throw std::invalid_argument("invalid AccelerationStructureBuilder initialization");
  }

  m_Resources        = &resources;
  m_Execution        = &execution;
  m_ScratchAlignment = scratchAlignment;
}

VkResult AccelerationStructureBuilder::Build(std::span<const AccelerationStructureBuild> builds, std::span<AccelerationStructure> accelerationStructures) const
{
  // Validation
  // Everything is checked before anything is allocated, so a bad batch leaves no partial state behind.

  if(m_Resources == nullptr || builds.empty() || builds.size() != accelerationStructures.size())
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for(size_t index = 0; index < builds.size(); ++index)
  {
    if(!builds[index].Ready() || accelerationStructures[index])
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  // Destinations
  // If any allocation fails, the destinations allocated so far are released so the caller gets back the empty handles it passed in.

  size_t allocatedCount = 0;
  for(size_t index = 0; index < builds.size(); ++index)
  {
    const VkResult result = m_Resources->CreateAccelerationStructure(accelerationStructures[index], builds[index].CreateInfo());

    if(result != VK_SUCCESS)
    {
      for(size_t allocated = 0; allocated < allocatedCount; ++allocated)
      {
        m_Resources->DestroyAccelerationStructure(accelerationStructures[allocated]);
      }
      return result;
    }

    ++allocatedCount;
  }

  // Scratch buffer
  // The builds run one after another, so a single scratch buffer sized for the largest build serves the whole batch.

  VkDeviceSize scratchSize = 0;
  for(const AccelerationStructureBuild& build : builds)
  {
    scratchSize = std::max(scratchSize, build.ScratchSize());
  }

  Buffer scratch;
  const VkResult scratchResult = m_Resources->CreateBuffer(scratch, scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, m_ScratchAlignment);

  if(scratchResult != VK_SUCCESS)
  {
    for(AccelerationStructure& accelerationStructure : accelerationStructures)
    {
      m_Resources->DestroyAccelerationStructure(accelerationStructure);
    }
    return scratchResult;
  }

  // Build submission
  // All builds are recorded into one blocking submission. Between builds a memory barrier marks the hand-off of the shared scratch memory from one build's writes to the next build's reads and writes.
  // A final barrier makes the results readable by later builds and by ray tracing shaders.
  // On failure the destinations are released; the scratch buffer releases itself when it goes out of scope.

  const auto releaseDestinations = [&] {
    for(AccelerationStructure& accelerationStructure : accelerationStructures)
    {
      m_Resources->DestroyAccelerationStructure(accelerationStructure);
    }
  };

  try
  {
    const CompletionPoint completion = m_Execution->ExecuteAndWait([&](VkCommandBuffer commandBuffer) {
      for(size_t index = 0; index < builds.size(); ++index)
      {
        builds[index].Record(commandBuffer, accelerationStructures[index].accel, scratch.address);

        if(index + 1 < builds.size())
        {
          const VkMemoryBarrier2 reuseScratch {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
          };

          const VkDependencyInfo dependency {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers    = &reuseScratch,
          };

          vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }
      }

      const VkMemoryBarrier2 makeReadable {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
      };

      const VkDependencyInfo dependency {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &makeReadable,
      };

      vkCmdPipelineBarrier2(commandBuffer, &dependency);
    });

    // ExecuteAndWait throws on every failure and otherwise returns the submission's nonzero timeline value, so there is nothing left to check.
    (void)completion;
  }
  catch(...)
  {
    releaseDestinations();
    throw;
  }

  m_Resources->DestroyBuffer(scratch);

  return VK_SUCCESS;
}

VkResult AccelerationStructureBuilder::Build(const AccelerationStructureBuild& build, AccelerationStructure& accelerationStructure) const
{
  return Build(std::span(&build, 1), std::span(&accelerationStructure, 1));
}

VkTransformMatrixKHR ToTransformMatrix(const float* columnMajorMatrix4x4)
{
  if(columnMajorMatrix4x4 == nullptr)
  {
    throw std::invalid_argument("transform matrix pointer is null");
  }

  // Column-major storage puts element (row, column) at column * 4 + row. Only the top three rows are copied; an affine transform's bottom row is implied.
  VkTransformMatrixKHR transform {};
  for(uint32_t row = 0; row < 3; ++row)
  {
    for(uint32_t column = 0; column < 4; ++column)
    {
      transform.matrix[row][column] = columnMajorMatrix4x4[column * 4 + row];
    }
  }

  return transform;
}

}  // namespace rtpt
