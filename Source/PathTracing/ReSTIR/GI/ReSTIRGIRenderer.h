#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "PathTracing/ReSTIR/Common/ReSTIRContext.h"
#include "PathTracing/ReSTIR/Common/ReSTIRRendererCommon.h"
#include "PathTracing/ReSTIR/Common/ReSTIRRenderUtils.h"
#include "PathTracing/ReSTIR/GI/ReSTIRGIResources.h"
#include "PathTracing/ReSTIR/GI/ReSTIRGISettings.h"
#include "nvvk/descriptors.hpp"

namespace nvapp
{
class Application;
}

namespace restir
{
class ReSTIRGIContext;
}

namespace nvsamples
{

class ReSTIRGIRenderer
{
public:
  using CreateInfo  = ReSTIRRendererCreateInfo;
  using RenderInput = ReSTIRRenderInput;

  explicit ReSTIRGIRenderer(const CreateInfo& createInfo);
  ~ReSTIRGIRenderer();

  void Initialize();
  void Destroy();
  bool IsReady() const;

  ReSTIRGISettings&       GetSettings();
  const ReSTIRGISettings& GetSettings() const;
  uint32_t                GetAccumulatedFrameCount() const;
  uint32_t                GetPipelineBounceLimit() const;
  void                    InvalidateHistory();

  nvvk::DescriptorPack&       GetDescriptorPack();
  const nvvk::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:
  using HistorySignature    = ReSTIRHistorySignature;
  using RayTracingPassState = ReSTIRRayTracingPassState;

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
  void EnsureGIContext(VkExtent2D viewportSize);
  void UpdateFrameDescriptors(const RenderInput& input);
  void ClearHistoryBuffers(VkCommandBuffer cmd);
  void RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRGIPushConstant& pushConstant);
  void RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRGIPushConstant& pushConstant);
  void RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRGIPushConstant& pushConstant);
  void RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRGIPushConstant& pushConstant);
  void UpdateParameterBuffer(uint32_t frameSetIndex, const shaderio::ReSTIRGIParameters& parameters);

  nvapp::Application*                   m_App       = nullptr;
  nvvk::ResourceAllocator*              m_Allocator = nullptr;
  uint32_t                              m_MaxTextureDescriptors = 0;
  uint32_t                              m_MaxBounceLimit        = 0;
  uint32_t                              m_PipelineBounceLimit   = 0;
  uint32_t                              m_AccumulatedFrames     = 0;
  bool                                  m_HistoryInvalidated    = true;
  bool                                  m_HasHistorySignature   = false;
  bool                                  m_NeedsHistoryClear     = true;

  ReSTIRGISettings                      m_Settings{};
  HistorySignature                      m_LastHistorySignature{};
  ReSTIRContext                         m_Context;
  ReSTIRGIResources                     m_Resources;
  std::unique_ptr<restir::ReSTIRGIContext> m_GiContext;
  std::vector<nvvk::Buffer>             m_ParameterBuffers;

  nvvk::DescriptorPack                  m_DescPack;
  VkPipelineLayout                      m_PipelineLayout = VK_NULL_HANDLE;
  RayTracingPassState                   m_InitialSamplingPass;
  RayTracingPassState                   m_FinalShadingPass;
  std::array<VkPipeline, static_cast<size_t>(ComputePass::eCount)> m_ComputePipelines{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples
