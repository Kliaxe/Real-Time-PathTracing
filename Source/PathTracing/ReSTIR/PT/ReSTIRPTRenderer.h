#pragma once

#include <array>
#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "PathTracing/ReSTIR/Common/ReSTIRContext.h"
#include "PathTracing/ReSTIR/Common/ReSTIRRendererCommon.h"
#include "PathTracing/ReSTIR/Common/ReSTIRRenderUtils.h"
#include "PathTracing/ReSTIR/Common/ReSTIRResources.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTSettings.h"
#include "PathTracing/PathTraceDenoiserResources.h"
#include "PathTracing/PathTraceNrdDenoiser.h"
#include "nvvk/descriptors.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

// CPU-side owner of the thesis ReSTIR PT pipeline. It builds the Vulkan
// objects, owns the history invalidation contract, and records the four-pass
// frame sequence.
class ReSTIRPTRenderer
{
public:
  using CreateInfo  = ReSTIRRendererCreateInfo;
  using RenderInput = ReSTIRRenderInput;

  explicit ReSTIRPTRenderer(const CreateInfo& createInfo);

  void Initialize();
  void Destroy();
  bool IsReady() const;

  ReSTIRPTSettings&       GetSettings();
  const ReSTIRPTSettings& GetSettings() const;
  uint32_t                GetAccumulatedFrameCount() const;
  uint32_t                GetPipelineBounceLimit() const;
  void                    InvalidateHistory();

  nvvk::DescriptorPack&       GetDescriptorPack();
  const nvvk::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:
  using HistorySignature   = ReSTIRHistorySignature;
  using DenoiserSignature  = ReSTIRDenoiserHistorySignature;
  using RayTracingPassState = ReSTIRRayTracingPassState;

  enum class ComputePass : uint32_t
  {
    eTemporal = 0,
    eSpatial,
    eCount,
  };

  void QueryRayTracingProperties();
  // The descriptor layout is shared by all ReSTIR PT passes so the CPU/GPU
  // contract stays stable while only the active pipeline changes.
  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateInitialSamplingPipeline();
  void CreateFinalShadingPipeline();
  void CreateComputePipelines();
  uint32_t BuildFrameFlags(bool enableTemporal, bool enableSpatial) const;
  void UpdateFrameDescriptors(const RenderInput& input);
  void RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  uint32_t                 m_MaxTextureDescriptors = 0;
  uint32_t                 m_MaxBounceLimit        = 0;
  uint32_t                 m_PipelineBounceLimit   = 0;
  uint32_t                 m_AccumulatedFrames     = 0;
  bool                     m_HistoryInvalidated    = true;
  bool                     m_HasHistorySignature   = false;
  bool                     m_HasDenoiserSignature  = false;

  ReSTIRPTSettings         m_Settings{};
  // Stored so render-time signature comparison can reset accumulated output
  // only when the accumulation contract changes.
  HistorySignature         m_LastHistorySignature{};
  DenoiserSignature        m_LastDenoiserSignature{};
  ReSTIRContext            m_Context;
  ReSTIRResources          m_Resources;
  // The NRD shell is shared with the ground-truth path tracer; PT plugs in by
  // producing the same guide and noisy-signal contract.
  PathTraceDenoiserResources m_DenoiserResources;
  PathTraceNrdDenoiser       m_NrdDenoiser;

  nvvk::DescriptorPack        m_DescPack;
  VkPipelineLayout            m_PipelineLayout = VK_NULL_HANDLE;
  RayTracingPassState         m_InitialSamplingPass;
  RayTracingPassState         m_FinalShadingPass;
  std::array<VkPipeline, static_cast<size_t>(ComputePass::eCount)> m_ComputePipelines{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples
