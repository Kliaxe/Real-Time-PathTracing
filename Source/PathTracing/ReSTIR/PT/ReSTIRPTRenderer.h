#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "PathTracing/ReSTIR/PT/ReSTIRPTParameterContext.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTRendererTypes.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTResources.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTSettings.h"
// Frame parity tracking and ray-tracing pass helpers are renderer-agnostic and
// shared with ReSTIR DI.
#include "PathTracing/ReSTIR/ReSTIRFrameContext.h"
#include "PathTracing/ReSTIR/ReSTIRRenderPassUtils.h"
// NRD integration is shared with the path tracer and ReSTIR DI; PT differs only in
// which pass produces the signals.
#include "Denoising/DenoiserResources.h"
#include "Denoising/NrdDenoiser.h"
#include "nvvk/descriptors.hpp"
#include "nvutils/profiler.hpp"
#include "nvvk/profiler_vk.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

// Real-Time PathTracing ReSTIR PT Enhanced renderer.
//
// Implements "ReSTIR PT Enhanced" (Lin, Kettunen, Wyman; I3D 2026) from the paper
// rather than porting an existing implementation. It is a sibling of
// ReSTIRDIRenderer, not a specialization: the two share infrastructure (resolve
// modes, frame parity, pass helpers) but own independent shader ABIs and pass
// sequences. In particular PT unifies direct and global illumination into one
// reservoir (Section 6.1), so it has no separate direct-lighting pass.
//
// CPU responsibility: own Vulkan state, update descriptors/parameters, and record
// the pass sequence. Shader responsibility: perform the resampling math.
//
// Current pass sequence is initial sampling (ray tracing) then final shading
// (compute). The temporal and spatial reuse passes arrive with the hybrid shift;
// until then the resampling mode selector has no reuse to enable, and the renderer
// is a 1spp path tracer routed through the reservoir plumbing. That configuration
// is the correctness gate: it must converge to the same image as the standalone
// path tracer.
class ReSTIRPTRenderer
{
public:
  using CreateInfo  = ReSTIRPTRendererCreateInfo;
  using RenderInput = ReSTIRPTRenderInput;

  explicit ReSTIRPTRenderer(const CreateInfo& createInfo);
  ~ReSTIRPTRenderer();

  void Initialize();
  void Destroy();
  bool IsReady() const;

  ReSTIRPTSettings&       GetSettings();
  const ReSTIRPTSettings& GetSettings() const;

  uint32_t GetAccumulatedFrameCount() const;
  uint32_t GetPipelineBounceLimit() const;
  // Bytes of reservoir storage currently allocated, surfaced in the UI because a
  // 64-byte reservoir per pixel per array dominates this renderer's memory use.
  VkDeviceSize GetReservoirMemoryUsage() const;
  void         InvalidateHistory();


  nvvk::DescriptorPack&       GetDescriptorPack();
  const nvvk::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:
  // Whether Section 6.2.2's compacted pre-pass runs this frame.
  bool UseSortedPrepass() const;

  using AccumulationSignature = ReSTIRPTAccumulationSignature;
  using DenoiserSignature     = ReSTIRPTDenoiserHistorySignature;
  using RayTracingPassState   = ReSTIRRayTracingPassState;

  struct FrameState
  {
    // Snapshot of decisions that must stay consistent while recording one frame.
    VkExtent2D            viewportSize{};
    AccumulationSignature accumulationSignature{};
    DenoiserSignature     denoiserSignature{};
    bool                  restirDebugActive = false;
    bool                  denoiseEnabled    = false;
    // Final shading writes NRD guide buffers only when NRD will consume them, so a
    // debug view - which replaces the beauty image - suppresses them.
    bool                  denoiserSignalsNeeded      = false;
    bool                  denoiserHistoryInvalidated = false;
  };

  void QueryRayTracingProperties();
  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateParameterBuffers();
  void CreateInitialSamplingPipeline();
  void CreateTemporalPipeline();
  void CreateSpatialPipeline();
  void CreateFinalShadingPipeline();

  bool       CanRender(const RenderInput& input) const;
  void       EnsureViewportResources(VkExtent2D viewportSize);
  void       EnsureParameterContext(VkExtent2D viewportSize);
  FrameState BeginFrame(const RenderInput& input, VkExtent2D viewportSize);

  shaderio::ReSTIRPTParameters   BuildShaderParameters();
  shaderio::ReSTIRPTPushConstant BuildPushConstant(const RenderInput& input, bool denoiserSignalsNeeded) const;

  void UpdateParameterBuffer(uint32_t frameSetIndex, const shaderio::ReSTIRPTParameters& parameters);
  void UpdateFrameDescriptors(const RenderInput& input);
  void PrepareDenoiser(const RenderInput& input, const FrameState& frameState);
  void RunDenoiserIfNeeded(const RenderInput& input, const FrameState& frameState);
  void PrepareStorageImages(const RenderInput& input, bool denoiserSignalsNeeded);
  void ClearHistoryIfNeeded(VkCommandBuffer cmd);
  void ClearHistoryBuffers(VkCommandBuffer cmd);
  void RecordPasses(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunSpatialPrepass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunDuplicationMapPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunLightTilePass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  // Section 6.2.2. Builds the sorted work list, then traces it indirectly.
  void RunSortedSpatialPrepass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void FinishFrame(const FrameState& frameState);

  // External owners give us the Vulkan application and allocator.
  nvapp::Application*      m_App                   = nullptr;
  nvvk::ResourceAllocator* m_Allocator             = nullptr;
  uint32_t                 m_MaxTextureDescriptors = 0;

  // The bounce limit comes from Vulkan ray recursion support, not just the UI.
  uint32_t m_PipelineBounceLimit = 0;
  // Accumulation is presentation history, separate from ReSTIR reuse history.
  uint32_t m_AccumulatedFrames = 0;
  // ReSTIR history must be cleared when previous reservoirs/surfaces no longer match.
  bool m_HistoryInvalidated       = true;
  bool m_HasAccumulationSignature = false;
  bool m_HasDenoiserSignature     = false;
  // GPU buffers are cleared lazily because the clear must be recorded into a command buffer.
  bool m_NeedsHistoryClear = true;

  ReSTIRPTSettings      m_Settings{};
  AccumulationSignature m_LastAccumulationSignature{};
  DenoiserSignature     m_LastDenoiserSignature{};

  ReSTIRFrameContext m_FrameContext;
  ReSTIRPTResources    m_Resources;
  // NRD input images and the denoiser itself, shared with the path tracer and DI.
  DenoiserResources    m_DenoiserResources;
  NrdDenoiser          m_NrdDenoiser;
  // Parameter context is recreated when viewport-dependent reservoir layout changes.
  std::unique_ptr<ReSTIRPTParameterContext> m_ParameterContext;
  // One mapped uniform buffer per nvpro frame set avoids CPU/GPU overwrite hazards.
  std::vector<nvvk::Buffer> m_ParameterBuffers;

  nvvk::DescriptorPack m_DescPack;
  VkPipelineLayout     m_PipelineLayout = VK_NULL_HANDLE;
  // Initial sampling traces the path tree; final shading only resolves reservoirs,
  // so unlike ReSTIR DI it needs no rays and is a compute pass.
  RayTracingPassState m_InitialSamplingPass;
  // Temporal reuse. A ray tracing pass rather than compute (which is what ReSTIR DI
  // uses) because the hybrid shift traces: one ray per replayed bounce plus one for
  // the reconnection, driven from a loop in ray generation.
  RayTracingPassState m_TemporalPass;
  // Spatial reuse. Reads the temporal output array and writes a third one, so a
  // pixel never reads a neighbour that another invocation is concurrently
  // rewriting - which is what the third reservoir array exists for.
  // Section 3 pre-pass: shifts each pixel's path into its paired partner's domain
  // so both partners can share the result. Ray tracing, not compute: recovering
  // the partner's surface and running the shift both trace.
  RayTracingPassState m_SpatialPrepass;
  RayTracingPassState m_SpatialPass;
  VkPipeline          m_FinalShadingPipeline = VK_NULL_HANDLE;
  // Section 5's correlation measure. Compute, like final shading: it only reads
  // reservoirs and writes a scalar per pixel, so it needs no rays.
  VkPipeline          m_DuplicationMapPipeline = VK_NULL_HANDLE;
  VkPipeline          m_LightTilePipeline      = VK_NULL_HANDLE;
  // Section 6.2.2: build the compacted work list, then publish its length.
  VkPipeline          m_PrepassClassifyPipeline = VK_NULL_HANDLE;
  VkPipeline          m_PrepassOffsetsPipeline  = VK_NULL_HANDLE;

  // Per-pass GPU timing. Wall-clock around the process cannot separate a pass from
  // thermal drift - the same configuration measured 6.7 and 14.5 ms/frame hours
  // apart on this machine - so a claim about a pass needs timestamps taken inside
  // the frame that produced it.

  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples
