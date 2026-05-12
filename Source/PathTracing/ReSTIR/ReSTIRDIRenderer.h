#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "PathTracing/ReSTIR/ReSTIRDIFrameContext.h"
#include "PathTracing/ReSTIR/ReSTIRDIRendererTypes.h"
#include "PathTracing/ReSTIR/ReSTIRDIRenderPassUtils.h"
#include "PathTracing/ReSTIR/ReSTIRDIResources.h"
#include "PathTracing/ReSTIR/ReSTIRDISettings.h"
#include "PathTracing/PathTraceDenoiserResources.h"
#include "PathTracing/PathTraceNrdDenoiser.h"
#include "nvvk/descriptors.hpp"

namespace nvapp
{
class Application;
}

namespace restir
{
class ReSTIRDIContext;
}

namespace nvsamples
{

class ReSTIRDIRenderer
{
public:
  using CreateInfo  = ReSTIRDIRendererCreateInfo;
  using RenderInput = ReSTIRDIRenderInput;

  explicit ReSTIRDIRenderer(const CreateInfo& createInfo);
  ~ReSTIRDIRenderer();

  void Initialize();
  void Destroy();
  bool IsReady() const;

  ReSTIRDISettings&       GetSettings();
  const ReSTIRDISettings& GetSettings() const;
  uint32_t                GetAccumulatedFrameCount() const;
  uint32_t                GetPipelineBounceLimit() const;
  void                    InvalidateHistory();

  nvvk::DescriptorPack&       GetDescriptorPack();
  const nvvk::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:
  using HistorySignature    = ReSTIRDIHistorySignature;
  using DenoiserSignature   = ReSTIRDIDenoiserHistorySignature;
  using RayTracingPassState = ReSTIRDIRayTracingPassState;

  enum class ComputePass : uint32_t
  {
    eTemporal = 0,
    eSpatial,
    eCount,
  };

  void QueryRayTracingProperties();
  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateParameterBuffers();
  void CreateInitialSamplingPipeline();
  void CreateFinalShadingPipeline();
  void CreateComputePipelines();
  void EnsureDIContext(VkExtent2D viewportSize);
  void UpdateFrameDescriptors(const RenderInput& input);
  void ClearHistoryBuffers(VkCommandBuffer cmd);
  void RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant);
  void RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant);
  void RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant);
  void RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant);
  void UpdateParameterBuffer(uint32_t frameSetIndex, const shaderio::ReSTIRDIParameters& parameters);

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  uint32_t                 m_MaxTextureDescriptors = 0;
  uint32_t                 m_MaxBounceLimit        = 0;
  uint32_t                 m_PipelineBounceLimit   = 0;
  uint32_t                 m_AccumulatedFrames     = 0;
  bool                     m_HistoryInvalidated    = true;
  bool                     m_HasHistorySignature   = false;
  bool                     m_HasDenoiserSignature  = false;
  bool                     m_NeedsHistoryClear     = true;

  ReSTIRDISettings         m_Settings{};
  HistorySignature         m_LastHistorySignature{};
  DenoiserSignature        m_LastDenoiserSignature{};
  ReSTIRDIFrameContext            m_Context;
  ReSTIRDIResources        m_Resources;
  PathTraceDenoiserResources m_DenoiserResources;
  PathTraceNrdDenoiser       m_NrdDenoiser;
  std::unique_ptr<restir::ReSTIRDIContext> m_DiContext;
  std::vector<nvvk::Buffer> m_ParameterBuffers;

  nvvk::DescriptorPack     m_DescPack;
  VkPipelineLayout         m_PipelineLayout = VK_NULL_HANDLE;
  RayTracingPassState      m_InitialSamplingPass;
  RayTracingPassState      m_FinalShadingPass;
  std::array<VkPipeline, static_cast<size_t>(ComputePass::eCount)> m_ComputePipelines{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples
