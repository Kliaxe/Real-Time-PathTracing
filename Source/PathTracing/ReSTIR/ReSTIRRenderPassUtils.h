#pragma once

#include <span>

#include <volk.h>

#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/ShaderBindingTable.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

// ReSTIRRayTracingPassState
// One ray tracing pass of the ReSTIR PT renderer: its pipeline and the shader binding table built against it.
// Kept together because the SBT is only meaningful for the pipeline it was created from, so they are created and destroyed as a pair.

struct ReSTIRRayTracingPassState
{
  // Ray tracing pipeline with the shared raygen/miss/hit stage layout.
  VkPipeline               pipeline = VK_NULL_HANDLE;

  // Maps TraceRay indices to this pipeline's shader groups.
  rtpt::ShaderBindingTable sbt;
};

inline bool IsReSTIRRayTracingPassReady(const ReSTIRRayTracingPassState& passState)
{
  return passState.pipeline != VK_NULL_HANDLE && passState.sbt.Storage();
}

// Pass creation
// Every ReSTIR PT ray tracing pass uses the same stage names and group layout, so only the SPIR-V, recursion depth, and debug name vary.

void CreateReSTIRRayTracingPass(rtpt::ResourceAllocator& resources, const rtpt::Diagnostics* diagnostics, const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rtProperties, VkPipelineLayout pipelineLayout, std::span<const uint32_t> spirv, uint32_t maxPipelineRayRecursionDepth, const char* debugName, ReSTIRRayTracingPassState& passState);

void DestroyReSTIRRayTracingPass(VkDevice device, ReSTIRRayTracingPassState& passState);

VkPipeline CreateReSTIRComputePipeline(VkDevice device, const rtpt::Diagnostics* diagnostics, VkPipelineLayout pipelineLayout, std::span<const uint32_t> spirv, const char* debugName);

// Image transitions
// Updates the accumulation image's recorded descriptor layout to GENERAL, so later descriptor writes and transitions start from the layout the command buffer actually leaves it in.
// Single storage images go through TransitionStorageImageForWrite in Framework/Vulkan/Barriers.h.

// Moves the accumulation image and the external output image to GENERAL in one barrier batch.
void TransitionReSTIRStorageImages(VkCommandBuffer cmd, rtpt::Image& accumulationImage, VkImage outputImage, VkPipelineStageFlags2 destinationStages);

// Pass recording
// Bind, push constants, and launch. Every pass binds descriptor set 0 of the shared pipeline layout, so a pass differs only in pipeline and launch size.

template <typename TPushConstant>
void TraceReSTIRRayTracingPass(VkCommandBuffer cmd, const ReSTIRRayTracingPassState& passState, VkPipelineLayout pipelineLayout, VkDescriptorSet descriptorSet, VkShaderStageFlags pushConstantStages, const TPushConstant& pushConstant, VkExtent2D viewportSize)
{
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, passState.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
  vkCmdPushConstants(cmd, pipelineLayout, pushConstantStages, 0, sizeof(TPushConstant), &pushConstant);

  // One ray generation invocation per viewport pixel.
  const rtpt::ShaderBindingTableRegions& regions = passState.sbt.Regions();
  vkCmdTraceRaysKHR(cmd, &regions.raygen, &regions.miss, &regions.hit, &regions.callable, viewportSize.width, viewportSize.height, 1);
}

template <typename TPushConstant>
void DispatchReSTIRComputePass(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout pipelineLayout, VkDescriptorSet descriptorSet, VkShaderStageFlags pushConstantStages, const TPushConstant& pushConstant, VkExtent2D viewportSize, uint32_t groupSize)
{
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
  vkCmdPushConstants(cmd, pipelineLayout, pushConstantStages, 0, sizeof(TPushConstant), &pushConstant);

  // Round the group count up so the edge pixels are covered; groupSize must match the shader's [numthreads].
  vkCmdDispatch(cmd, (viewportSize.width + groupSize - 1u) / groupSize, (viewportSize.height + groupSize - 1u) / groupSize, 1);
}

}  // namespace rtpt
