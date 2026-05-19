#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "Denoising/DenoiserResources.h"
#include "Denoising/NrdDenoiser.h"
#include "PathTracing/ReSTIR/ReSTIRDIFrameContext.h"
#include "PathTracing/ReSTIR/ReSTIRDIRendererTypes.h"
#include "PathTracing/ReSTIR/ReSTIRDIRenderPassUtils.h"
#include "PathTracing/ReSTIR/ReSTIRDIResources.h"
#include "PathTracing/ReSTIR/ReSTIRDISettings.h"
#include "nvvk/descriptors.hpp"

namespace nvapp
{
class Application;
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
  using AccumulationSignature = ReSTIRDIAccumulationSignature;
  using DenoiserSignature     = ReSTIRDIDenoiserHistorySignature;
  using RayTracingPassState   = ReSTIRDIRayTracingPassState;

  struct FrameState
  {
    VkExtent2D            viewportSize{};
    AccumulationSignature accumulationSignature{};
    DenoiserSignature     denoiserSignature{};
    bool                  denoiseEnabled = false;
    bool                  restirDebugActive = false;
    bool                  denoiserSignalsNeeded = false;
    bool                  denoiserHistoryInvalidated = false;
  };

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
  bool CanRender(const RenderInput& input) const;
  void EnsureViewportResources(VkExtent2D viewportSize);
  FrameState BeginReSTIRFrame(const RenderInput& input, VkExtent2D viewportSize);
  void PrepareDenoiser(const RenderInput& input, const FrameState& frameState);
  shaderio::ReSTIRDIParameters BuildShaderParameters();
  void PrepareStorageImages(const RenderInput& input, bool denoiserSignalsNeeded);
  void ClearHistoryIfNeeded(VkCommandBuffer cmd);
  shaderio::ReSTIRDIPushConstant BuildPushConstant(const RenderInput& input, bool denoiserSignalsNeeded) const;
  void RecordReSTIRPasses(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant);
  void RunDenoiserIfNeeded(const RenderInput& input, const FrameState& frameState);
  void FinishFrame(const FrameState& frameState);
  void EnsureParameterContext(VkExtent2D viewportSize);
  void UpdateFrameDescriptors(const RenderInput& input);
  void ClearHistoryBuffers(VkCommandBuffer cmd);
  void RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant);
  void RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant);
  void RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant);
  void RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant);
  void UpdateParameterBuffer(uint32_t frameSetIndex, const shaderio::ReSTIRDIParameters& parameters);

  // External owners give us the Vulkan application and allocator.
  nvapp::Application*      m_App                   = nullptr;
  nvvk::ResourceAllocator* m_Allocator             = nullptr;
  uint32_t                 m_MaxTextureDescriptors = 0;

  // Render state that changes as the camera, scene, or settings change.
  uint32_t m_PipelineBounceLimit      = 0;
  uint32_t m_AccumulatedFrames        = 0;
  bool     m_HistoryInvalidated       = true;
  bool     m_HasAccumulationSignature = false;
  bool     m_HasDenoiserSignature     = false;
  bool     m_NeedsHistoryClear        = true;

  ReSTIRDISettings      m_Settings{};
  AccumulationSignature m_LastAccumulationSignature{};
  DenoiserSignature     m_LastDenoiserSignature{};

  // ReSTIR owns one frame context, one parameter context, and the GPU buffers.
  ReSTIRDIFrameContext                      m_FrameContext;
  ReSTIRDIResources                         m_Resources;
  std::unique_ptr<ReSTIRDIParameterContext> m_ParameterContext;
  std::vector<nvvk::Buffer>                 m_ParameterBuffers;

  // NRD is optional output processing, shared with the path tracer.
  DenoiserResources m_DenoiserResources;
  NrdDenoiser       m_NrdDenoiser;

  nvvk::DescriptorPack     m_DescPack;
  VkPipelineLayout         m_PipelineLayout = VK_NULL_HANDLE;
  RayTracingPassState      m_InitialSamplingPass;
  RayTracingPassState      m_FinalShadingPass;
  std::array<VkPipeline, static_cast<size_t>(ComputePass::eCount)> m_ComputePipelines{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples
