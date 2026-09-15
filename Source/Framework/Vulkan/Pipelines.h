#pragma once

#include <span>
#include <string_view>
#include <vector>

#include <volk.h>

namespace rtpt
{

// ShaderModule
// Move-only owner of a VkShaderModule.
// Modules are only needed while a pipeline is being created, so the pipeline helpers below create one locally and let it destroy itself on return.

class ShaderModule
{
public:

  ShaderModule() = default;
  ShaderModule(const ShaderModule&)            = delete;
  ShaderModule& operator=(const ShaderModule&) = delete;
  ShaderModule(ShaderModule&& other) noexcept;
  ShaderModule& operator=(ShaderModule&& other) noexcept;
  ~ShaderModule();

  VkResult Initialize(VkDevice device, std::span<const uint32_t> spirv);
  void Destroy();

  [[nodiscard]] VkShaderModule Get() const noexcept { return m_Module; }

private:

  // Device that owns the module.
  VkDevice       m_Device = VK_NULL_HANDLE;
  // The module. Null until Initialize succeeds.
  VkShaderModule m_Module = VK_NULL_HANDLE;
};

// The output pipeline must be null on entry.
VkResult CreateComputePipeline(VkDevice device, VkPipelineLayout layout, std::span<const uint32_t> spirv, VkPipeline& pipeline, std::string_view entryPoint = "main");

// RayTracingShaderStage
// One ray tracing stage taken from a shared SPIR-V module: the stage kind and its entry point name.

struct RayTracingShaderStage
{
  // Ray generation, miss, closest hit, and so on.
  VkShaderStageFlagBits stage = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
  // Entry point in the module. Must be non-empty and outlive pipeline creation.
  const char*           entryPoint = nullptr;
};

// Every stage comes from the same SPIR-V module, selected by entry point. The output pipeline must be null on entry.
VkResult CreateRayTracingPipeline(VkDevice device, VkPipelineLayout layout, std::span<const uint32_t> spirv, std::span<const RayTracingShaderStage> shaderStages, std::span<const VkRayTracingShaderGroupCreateInfoKHR> groups, uint32_t maxRecursionDepth, VkPipeline& pipeline);

}  // namespace rtpt
