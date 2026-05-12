#pragma once

#include <volk/volk.h>

#include "PathTracing/ReSTIR/ReSTIRDIRendererTypes.h"
#include "Shaders/ShaderIo.h"
#include "nvvk/check_error.hpp"
#include "nvvk/resources.hpp"
#include "nvvk/sbt_generator.hpp"

namespace nvvk
{
class ResourceAllocator;
}

namespace nvsamples
{

struct ReSTIRDIRayTracingPassState
{
  VkPipeline                  pipeline = VK_NULL_HANDLE;
  nvvk::SBTGenerator          sbtGenerator;
  nvvk::Buffer                sbtBuffer;
  nvvk::SBTGenerator::Regions sbtRegions{};
};

inline bool IsReSTIRDIRayTracingPassReady(const ReSTIRDIRayTracingPassState& passState)
{
  return passState.pipeline != VK_NULL_HANDLE && passState.sbtBuffer.buffer != VK_NULL_HANDLE;
}

void CreateReSTIRDIRayTracingPass(nvvk::ResourceAllocator* allocator,
                                const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rtProperties,
                                VkPipelineLayout pipelineLayout,
                                const VkShaderModuleCreateInfo& shaderCode,
                                uint32_t maxPipelineRayRecursionDepth,
                                ReSTIRDIRayTracingPassState& passState);

void DestroyReSTIRDIRayTracingPass(nvvk::ResourceAllocator* allocator, ReSTIRDIRayTracingPassState& passState);

VkPipeline CreateReSTIRDIComputePipeline(nvvk::ResourceAllocator* allocator,
                                       VkPipelineLayout pipelineLayout,
                                       const VkShaderModuleCreateInfo& shaderCode);

void TransitionReSTIRDIStorageImages(VkCommandBuffer cmd, nvvk::Image& accumulationImage, VkImage outputImage,
                                   VkPipelineStageFlags2 destinationStages);

template <typename TPushConstant>
void TraceReSTIRDIRayTracingPass(VkCommandBuffer cmd,
                               const ReSTIRDIRayTracingPassState& passState,
                               VkPipelineLayout pipelineLayout,
                               VkDescriptorSet descriptorSet,
                               VkShaderStageFlags pushConstantStages,
                               const TPushConstant& pushConstant,
                               VkExtent2D viewportSize)
{
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, passState.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
  vkCmdPushConstants(cmd, pipelineLayout, pushConstantStages, 0, sizeof(TPushConstant), &pushConstant);
  vkCmdTraceRaysKHR(cmd, &passState.sbtRegions.raygen, &passState.sbtRegions.miss, &passState.sbtRegions.hit, &passState.sbtRegions.callable,
                    viewportSize.width, viewportSize.height, 1);
}

template <typename TPushConstant>
void DispatchReSTIRDIComputePass(VkCommandBuffer cmd,
                               VkPipeline pipeline,
                               VkPipelineLayout pipelineLayout,
                               VkDescriptorSet descriptorSet,
                               VkShaderStageFlags pushConstantStages,
                               const TPushConstant& pushConstant,
                               VkExtent2D viewportSize,
                               uint32_t groupSize)
{
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
  vkCmdPushConstants(cmd, pipelineLayout, pushConstantStages, 0, sizeof(TPushConstant), &pushConstant);

  const uint32_t groupCountX = (viewportSize.width + groupSize - 1u) / groupSize;
  const uint32_t groupCountY = (viewportSize.height + groupSize - 1u) / groupSize;
  vkCmdDispatch(cmd, groupCountX, groupCountY, 1);
}

}  // namespace nvsamples
