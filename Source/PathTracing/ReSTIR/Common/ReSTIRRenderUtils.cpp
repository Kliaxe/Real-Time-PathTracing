#include <array>

#include "ReSTIRRenderUtils.h"

#include <vector>

#include <nvvk/resource_allocator.hpp>

namespace nvsamples
{

void CreateReSTIRRayTracingPass(nvvk::ResourceAllocator* allocator,
                                const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rtProperties,
                                VkPipelineLayout pipelineLayout,
                                const VkShaderModuleCreateInfo& shaderCode,
                                uint32_t maxPipelineRayRecursionDepth,
                                ReSTIRRayTracingPassState& passState)
{
  VkDevice device = allocator->getDevice();

  VkShaderModule shaderModule = VK_NULL_HANDLE;
  NVVK_CHECK(vkCreateShaderModule(device, &shaderCode, nullptr, &shaderModule));

  enum ShaderStageIndex : uint32_t
  {
    eRaygen = 0,
    eMiss,
    eShadowMiss,
    eAnyHit,
    eShadowAnyHit,
    eClosestHit,
    eStageCount,
  };

  std::array<VkPipelineShaderStageCreateInfo, eStageCount> stages{};
  for(VkPipelineShaderStageCreateInfo& stage : stages)
  {
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.module = shaderModule;
  }

  stages[eRaygen].stage       = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  stages[eRaygen].pName       = "rgenMain";
  stages[eMiss].stage         = VK_SHADER_STAGE_MISS_BIT_KHR;
  stages[eMiss].pName         = "rmissMain";
  stages[eShadowMiss].stage   = VK_SHADER_STAGE_MISS_BIT_KHR;
  stages[eShadowMiss].pName   = "shadowMissMain";
  stages[eAnyHit].stage       = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
  stages[eAnyHit].pName       = "rahitMain";
  stages[eShadowAnyHit].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
  stages[eShadowAnyHit].pName = "shadowAnyHitMain";
  stages[eClosestHit].stage   = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
  stages[eClosestHit].pName   = "rchitMain";

  VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
  group.anyHitShader       = VK_SHADER_UNUSED_KHR;
  group.closestHitShader   = VK_SHADER_UNUSED_KHR;
  group.generalShader      = VK_SHADER_UNUSED_KHR;
  group.intersectionShader = VK_SHADER_UNUSED_KHR;

  std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
  groups.reserve(5);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eRaygen;
  groups.push_back(group);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eMiss;
  groups.push_back(group);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eShadowMiss;
  groups.push_back(group);

  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.anyHitShader     = eAnyHit;
  group.closestHitShader = eClosestHit;
  groups.push_back(group);

  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.anyHitShader     = eShadowAnyHit;
  group.closestHitShader = VK_SHADER_UNUSED_KHR;
  groups.push_back(group);

  const VkRayTracingPipelineCreateInfoKHR pipelineInfo{
      .sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
      .stageCount                   = static_cast<uint32_t>(stages.size()),
      .pStages                      = stages.data(),
      .groupCount                   = static_cast<uint32_t>(groups.size()),
      .pGroups                      = groups.data(),
      .maxPipelineRayRecursionDepth = maxPipelineRayRecursionDepth,
      .layout                       = pipelineLayout,
  };

  NVVK_CHECK(vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &passState.pipeline));

  passState.sbtGenerator.init(device, rtProperties);
  const size_t sbtBufferSize = passState.sbtGenerator.calculateSBTBufferSize(passState.pipeline, pipelineInfo);
  NVVK_CHECK(allocator->createBuffer(passState.sbtBuffer, sbtBufferSize,
                                     VK_BUFFER_USAGE_2_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                     VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                     passState.sbtGenerator.getBufferAlignment()));
  NVVK_CHECK(passState.sbtGenerator.populateSBTBuffer(passState.sbtBuffer.address, passState.sbtBuffer.bufferSize,
                                                      passState.sbtBuffer.mapping));
  passState.sbtRegions = passState.sbtGenerator.getSBTRegions(0);

  vkDestroyShaderModule(device, shaderModule, nullptr);
}

void DestroyReSTIRRayTracingPass(nvvk::ResourceAllocator* allocator, ReSTIRRayTracingPassState& passState)
{
  if(allocator == nullptr)
  {
    return;
  }

  VkDevice device = allocator->getDevice();
  allocator->destroyBuffer(passState.sbtBuffer);
  passState.sbtBuffer = {};
  passState.sbtGenerator.deinit();
  passState.sbtRegions = {};
  vkDestroyPipeline(device, passState.pipeline, nullptr);
  passState.pipeline = VK_NULL_HANDLE;
}

VkPipeline CreateReSTIRComputePipeline(nvvk::ResourceAllocator* allocator,
                                       VkPipelineLayout pipelineLayout,
                                       const VkShaderModuleCreateInfo& shaderCode)
{
  VkShaderModule shaderModule = VK_NULL_HANDLE;
  NVVK_CHECK(vkCreateShaderModule(allocator->getDevice(), &shaderCode, nullptr, &shaderModule));

  const VkPipelineShaderStageCreateInfo shaderStage{
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shaderModule,
      .pName  = "main",
  };
  const VkComputePipelineCreateInfo pipelineInfo{
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = shaderStage,
      .layout = pipelineLayout,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  NVVK_CHECK(vkCreateComputePipelines(allocator->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
  vkDestroyShaderModule(allocator->getDevice(), shaderModule, nullptr);
  return pipeline;
}

void TransitionReSTIRStorageImages(VkCommandBuffer cmd, nvvk::Image& accumulationImage, VkImage outputImage,
                                   VkPipelineStageFlags2 destinationStages)
{
  std::array<VkImageMemoryBarrier2, 2> imageBarriers{};
  uint32_t                             imageBarrierCount = 0;

  if(accumulationImage.descriptor.imageLayout != VK_IMAGE_LAYOUT_GENERAL)
  {
    imageBarriers[imageBarrierCount++] = VkImageMemoryBarrier2{
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask  = destinationStages,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout     = accumulationImage.descriptor.imageLayout,
        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .image         = accumulationImage.image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
    };
    accumulationImage.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  imageBarriers[imageBarrierCount++] = VkImageMemoryBarrier2{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask  = destinationStages,
      .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .image         = outputImage,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
  };

  const VkDependencyInfo dependencyInfo{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = imageBarrierCount,
      .pImageMemoryBarriers    = imageBarriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

}  // namespace nvsamples
