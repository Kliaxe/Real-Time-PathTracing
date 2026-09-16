#include "ReSTIRRenderPassUtils.h"

#include <array>
#include <vector>

#include "Framework/Vulkan/Pipelines.h"

namespace rtpt
{

void CreateReSTIRRayTracingPass(rtpt::ResourceAllocator& resources, const rtpt::Diagnostics* diagnostics, const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rtProperties, VkPipelineLayout pipelineLayout, std::span<const uint32_t> spirv, uint32_t maxPipelineRayRecursionDepth, const char* debugName, ReSTIRRayTracingPassState& passState)
{
  // Shader stages
  // Every ReSTIR PT library exports the same entry points: a primary miss and hit pair for path rays, and a shadow miss and any-hit pair for visibility rays.

  enum ShaderStageIndex : uint32_t
  {
    eRaygen,
    eMiss,
    eShadowMiss,
    eAnyHit,
    eShadowAnyHit,
    eClosestHit,
    eStageCount,
  };

  constexpr std::array<rtpt::RayTracingShaderStage, eStageCount> stages { {
      { VK_SHADER_STAGE_RAYGEN_BIT_KHR, "rgenMain" },
      { VK_SHADER_STAGE_MISS_BIT_KHR, "rmissMain" },
      { VK_SHADER_STAGE_MISS_BIT_KHR, "shadowMissMain" },
      { VK_SHADER_STAGE_ANY_HIT_BIT_KHR, "rahitMain" },
      { VK_SHADER_STAGE_ANY_HIT_BIT_KHR, "shadowAnyHitMain" },
      { VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "rchitMain" },
  } };

  // Shader groups
  // One template is edited and appended per group, so each step only changes the fields that differ from the previous group.
  // Group order is the SBT order below: raygen 0, miss 1-2, hit 3-4. The shadow hit group has no closest-hit shader because visibility rays only need to know whether something blocks them.

  VkRayTracingShaderGroupCreateInfoKHR group { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };

  group.anyHitShader       = VK_SHADER_UNUSED_KHR;
  group.closestHitShader   = VK_SHADER_UNUSED_KHR;
  group.generalShader      = VK_SHADER_UNUSED_KHR;
  group.intersectionShader = VK_SHADER_UNUSED_KHR;

  std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
  groups.reserve(5);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eRaygen;
  groups.push_back(group);

  group.generalShader = eMiss;
  groups.push_back(group);

  group.generalShader = eShadowMiss;
  groups.push_back(group);

  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.anyHitShader     = eAnyHit;
  group.closestHitShader = eClosestHit;
  groups.push_back(group);

  group.anyHitShader     = eShadowAnyHit;
  group.closestHitShader = VK_SHADER_UNUSED_KHR;
  groups.push_back(group);

  // Pipeline and SBT
  // The recursion depth comes from the caller, which owns the budget its pass's shaders were written for.

  rtpt::CheckVk(rtpt::CreateRayTracingPipeline(resources.Device(), pipelineLayout, spirv, stages, groups, maxPipelineRayRecursionDepth, passState.pipeline), "CreateRayTracingPipeline(ReSTIR PT)");

  constexpr std::array<uint32_t, 1> raygen { 0 };
  constexpr std::array<uint32_t, 2> miss { 1, 2 };
  constexpr std::array<uint32_t, 2> hit { 3, 4 };

  rtpt::CheckVk(passState.sbt.Initialize(resources, passState.pipeline, 5, rtProperties, { .raygen = raygen, .miss = miss, .hit = hit, .callable = {} }), "ShaderBindingTable::Initialize(ReSTIR PT)");

  if(diagnostics != nullptr)
  {
    diagnostics->SetObjectName(resources.Device(), VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(passState.pipeline), debugName);
  }
}

void DestroyReSTIRRayTracingPass(VkDevice device, ReSTIRRayTracingPassState& passState)
{
  // The SBT is released first because it was built from the pipeline.
  passState.sbt.Destroy();

  if(passState.pipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(device, passState.pipeline, nullptr);
  }

  passState.pipeline = VK_NULL_HANDLE;
}

VkPipeline CreateReSTIRComputePipeline(VkDevice device, const rtpt::Diagnostics* diagnostics, VkPipelineLayout pipelineLayout, std::span<const uint32_t> spirv, const char* debugName)
{
  VkPipeline pipeline = VK_NULL_HANDLE;

  rtpt::CheckVk(rtpt::CreateComputePipeline(device, pipelineLayout, spirv, pipeline), "CreateComputePipeline(ReSTIR PT)");

  if(diagnostics != nullptr)
  {
    diagnostics->SetObjectName(device, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline), debugName);
  }

  return pipeline;
}

void TransitionReSTIRStorageImages(VkCommandBuffer cmd, rtpt::Image& accumulationImage, VkImage outputImage, VkPipelineStageFlags2 destinationStages)
{
  std::array<VkImageMemoryBarrier2, 2> barriers {};
  uint32_t count = 0;

  // Accumulation image
  // Same rule as TransitionStorageImageForWrite's eOrderAfterPreviousWrites: accumulated history must survive, so an image with contents is ordered after every earlier writer.

  const VkImageLayout accumulationOldLayout = accumulationImage.descriptor.imageLayout;
  const bool accumulationHasContents = accumulationOldLayout != VK_IMAGE_LAYOUT_UNDEFINED;

  barriers[count++] = {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask     = accumulationHasContents ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask    = accumulationHasContents ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_NONE,
      .dstStageMask     = destinationStages,
      .dstAccessMask    = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout        = accumulationOldLayout,
      .newLayout        = VK_IMAGE_LAYOUT_GENERAL,
      .image            = accumulationImage.image,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
  };

  accumulationImage.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

  // Output image
  // The output target is owned by the caller and entered from UNDEFINED every frame, so its previous contents are discarded and the shaders only need write access.

  barriers[count++] = {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .dstStageMask     = destinationStages,
      .dstAccessMask    = VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout        = VK_IMAGE_LAYOUT_GENERAL,
      .image            = outputImage,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
  };

  const VkDependencyInfo dependency {
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = count,
      .pImageMemoryBarriers    = barriers.data(),
  };

  vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace rtpt
