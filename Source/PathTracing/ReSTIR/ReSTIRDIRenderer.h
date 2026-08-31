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

// Real-Time PathTracing ReSTIR DI renderer. It records the initial-sampling, temporal,
// spatial, and final-shading passes, then optionally feeds the result through
// the shared NRD denoising path.
// CPU responsibility: own Vulkan state, update descriptors/parameters, and
// record the pass sequence. Shader responsibility: perform the ReSTIR math.
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
    // Snapshot of decisions that must stay consistent while recording one frame.
    VkExtent2D            viewportSize{};
    AccumulationSignature accumulationSignature{};
    DenoiserSignature     denoiserSignature{};
    // Denoising is disabled while ReSTIR debug views own the output image.
    bool                  denoiseEnabled = false;
    bool                  restirDebugActive = false;
    // Final shading writes NRD guide buffers only when NRD will consume them.
    bool                  denoiserSignalsNeeded = false;
    bool                  denoiserHistoryInvalidated = false;
  };

  enum class ComputePass : uint32_t
  {
    // Initial and final passes are ray tracing pipelines; only reuse passes are compute.
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
  // The bounce limit comes from Vulkan ray recursion support, not just the UI.
  uint32_t m_PipelineBounceLimit      = 0;
  // Accumulation is presentation/reference history, separate from ReSTIR reuse history.
  uint32_t m_AccumulatedFrames        = 0;
  // ReSTIR history must be cleared when previous reservoirs/surfaces no longer match.
  bool     m_HistoryInvalidated       = true;
  bool     m_HasAccumulationSignature = false;
  bool     m_HasDenoiserSignature     = false;
  // GPU buffers are cleared lazily because the clear must be recorded into a command buffer.
  bool     m_NeedsHistoryClear        = true;

  ReSTIRDISettings      m_Settings{};
  // Signatures are compact CPU-side keys for "can old history still be trusted?"
  AccumulationSignature m_LastAccumulationSignature{};
  DenoiserSignature     m_LastDenoiserSignature{};

  // ReSTIR owns one frame context, one parameter context, and the GPU buffers.
  ReSTIRDIFrameContext                      m_FrameContext;
  ReSTIRDIResources                         m_Resources;
  // Parameter context is recreated when viewport-dependent reservoir layout changes.
  std::unique_ptr<ReSTIRDIParameterContext> m_ParameterContext;
  // One mapped uniform buffer per nvpro frame set avoids CPU/GPU overwrite hazards.
  std::vector<nvvk::Buffer>                 m_ParameterBuffers;

  // NRD output processing, shared with the path tracer.
  DenoiserResources m_DenoiserResources;
  NrdDenoiser       m_NrdDenoiser;

  nvvk::DescriptorPack     m_DescPack;
  VkPipelineLayout         m_PipelineLayout = VK_NULL_HANDLE;
  // Initial sampling traces primary rays; final shading traces optional secondary paths.
  RayTracingPassState      m_InitialSamplingPass;
  RayTracingPassState      m_FinalShadingPass;
  // Temporal and spatial reuse are compute passes between the two ray tracing passes.
  std::array<VkPipeline, static_cast<size_t>(ComputePass::eCount)> m_ComputePipelines{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples
