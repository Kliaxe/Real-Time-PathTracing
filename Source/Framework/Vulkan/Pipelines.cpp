#include "Pipelines.h"

#include <string>
#include <utility>

namespace rtpt
{

ShaderModule::ShaderModule(ShaderModule&& other) noexcept
{
  *this = std::move(other);
}

ShaderModule& ShaderModule::operator=(ShaderModule&& other) noexcept
{
  if(this != &other)
  {
    Destroy();

    m_Device = std::exchange(other.m_Device, VK_NULL_HANDLE);
    m_Module = std::exchange(other.m_Module, VK_NULL_HANDLE);
  }

  return *this;
}

ShaderModule::~ShaderModule()
{
  Destroy();
}

VkResult ShaderModule::Initialize(VkDevice device, std::span<const uint32_t> spirv)
{
  if(device == VK_NULL_HANDLE || spirv.empty() || m_Device != VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  // Vulkan takes the SPIR-V size in bytes, not words.
  const VkShaderModuleCreateInfo createInfo {
    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = spirv.size_bytes(),
    .pCode    = spirv.data(),
  };

  const VkResult result = vkCreateShaderModule(device, &createInfo, nullptr, &m_Module);

  if(result == VK_SUCCESS)
  {
    m_Device = device;
  }

  return result;
}

void ShaderModule::Destroy()
{
  if(m_Module != VK_NULL_HANDLE)
  {
    vkDestroyShaderModule(m_Device, m_Module, nullptr);
  }

  m_Module = VK_NULL_HANDLE;
  m_Device = VK_NULL_HANDLE;
}

VkResult CreateComputePipeline(VkDevice device, VkPipelineLayout layout, std::span<const uint32_t> spirv, VkPipeline& pipeline, std::string_view entryPoint)
{
  // A non-null output pipeline would be overwritten and leak.
  if(device == VK_NULL_HANDLE || layout == VK_NULL_HANDLE || pipeline != VK_NULL_HANDLE || entryPoint.empty())
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  // The module only has to outlive pipeline creation, so it is destroyed on return.
  ShaderModule module;
  VkResult result = module.Initialize(device, spirv);

  if(result != VK_SUCCESS)
  {
    return result;
  }

  // Pipeline
  // Vulkan consumes the entry-point name during pipeline creation. A string_view is not null-terminated, so a std::string copy is kept alive for the call.

  const std::string entryPointString(entryPoint);

  const VkComputePipelineCreateInfo createInfo {
    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage = {
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = module.Get(),
      .pName  = entryPointString.c_str(),
    },
    .layout = layout,
  };

  result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &createInfo, nullptr, &pipeline);

  return result;
}

VkResult CreateRayTracingPipeline(VkDevice device, VkPipelineLayout layout, std::span<const uint32_t> spirv, std::span<const RayTracingShaderStage> shaderStages, std::span<const VkRayTracingShaderGroupCreateInfoKHR> groups, uint32_t maxRecursionDepth, VkPipeline& pipeline)
{
  // A non-null output pipeline would be overwritten and leak.
  if(device == VK_NULL_HANDLE || layout == VK_NULL_HANDLE || shaderStages.empty() || groups.empty() || maxRecursionDepth == 0 || pipeline != VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  // The module only has to outlive pipeline creation, so it is destroyed on return.
  ShaderModule module;
  VkResult result = module.Initialize(device, spirv);

  if(result != VK_SUCCESS)
  {
    return result;
  }

  // Stages
  // Every stage references the same module and differs only by stage kind and entry point.

  std::vector<VkPipelineShaderStageCreateInfo> stages;
  stages.reserve(shaderStages.size());

  for(const RayTracingShaderStage& shaderStage : shaderStages)
  {
    if(shaderStage.entryPoint == nullptr || shaderStage.entryPoint[0] == '\0')
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    stages.push_back(VkPipelineShaderStageCreateInfo {
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = shaderStage.stage,
      .module = module.Get(),
      .pName  = shaderStage.entryPoint,
    });
  }

  // Pipeline
  // Groups index into the stage list in the order given.

  const VkRayTracingPipelineCreateInfoKHR createInfo {
    .sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
    .stageCount                   = static_cast<uint32_t>(stages.size()),
    .pStages                      = stages.data(),
    .groupCount                   = static_cast<uint32_t>(groups.size()),
    .pGroups                      = groups.data(),
    .maxPipelineRayRecursionDepth = maxRecursionDepth,
    .layout                       = layout,
  };

  return vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &createInfo, nullptr, &pipeline);
}

}  // namespace rtpt
