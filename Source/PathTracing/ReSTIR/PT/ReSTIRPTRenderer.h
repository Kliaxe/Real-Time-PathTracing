#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <volk.h>

#include "PathTracing/ReSTIR/PT/ReSTIRPTParameterContext.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTRendererTypes.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTResources.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTSettings.h"
// Frame parity tracking and ray-tracing pass helpers, kept separate from the renderer because they are ordinary Vulkan plumbing with no ReSTIR PT specifics.
#include "PathTracing/ReSTIR/ReSTIRFrameContext.h"
#include "PathTracing/ReSTIR/ReSTIRRenderPassUtils.h"
// Accumulation and NRD history are shared with the reference path tracer; the renderers differ only in which pass produces the signals.
#include "PathTracing/Common/ResolveHistory.h"
#include "Framework/Vulkan/Descriptors.h"

namespace rtpt
{

// ReSTIRPTRenderer
// Real-Time PathTracing ReSTIR PT Enhanced renderer.
// Implements "ReSTIR PT Enhanced" (Lin, Kettunen, Wyman; I3D 2026) from the paper rather than porting an existing implementation. Direct and global illumination share one reservoir (Section 6.1), so there is no separate direct-lighting pass.
// CPU responsibility: own Vulkan state, update descriptors/parameters, and record the pass sequence. Shader responsibility: perform the resampling math.
// Pass sequence: light tiles, initial sampling, temporal reuse, the spatial pre-pass and spatial reuse, final shading, and the duplication map. Every reuse pass is optional; with all of them off the renderer is a 1spp path tracer routed through the reservoir plumbing, which is the correctness gate - that configuration must converge to the same image as the reference path tracer.
// Per-pass GPU time is recorded through RenderInput's optional profiler: one "ReSTIR PT/<pass>" timestamp scope around each pass that runs this frame. Wall-clock around the process cannot separate a pass from thermal drift - the same configuration measured 6.7 and 14.5 ms/frame hours apart on this machine - so a claim about a pass needs those in-frame timestamps (headless --profile-output, or the Profiler section of the UI).

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
  uint32_t GetBounceLimit() const;

  // Bytes of reservoir storage currently allocated, surfaced in the UI because a 64-byte reservoir per pixel per array dominates this renderer's memory use.
  VkDeviceSize GetReservoirMemoryUsage() const;

  void         InvalidateHistory();

  rtpt::DescriptorPack&       GetDescriptorPack();
  const rtpt::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:

  // Whether Section 6.2.2's compacted pre-pass runs this frame.
  bool UseSortedPrepass() const;

  using RayTracingPassState = ReSTIRRayTracingPassState;

  // Creation
  // Called once from Initialize, in an order where each step only depends on the ones before it.

  void QueryRayTracingProperties();
  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateParameterBuffers();
  void CreateInitialSamplingPipeline();
  void CreateTemporalPipeline();
  void CreateSpatialPipeline();
  void CreateFinalShadingPipeline();

  // Frame setup

  bool       CanRender(const RenderInput& input) const;
  void       EnsureViewportResources(VkExtent2D viewportSize);
  void       EnsureParameterContext(VkExtent2D viewportSize);
  ResolveHistory::FrameState BeginFrame(const RenderInput& input, VkExtent2D viewportSize);

  shaderio::ReSTIRPTParameters   BuildShaderParameters();
  shaderio::ReSTIRPTPushConstant BuildPushConstant(const RenderInput& input, const ResolveHistory::FrameState& frameState) const;

  // Frame recording

  void UpdateParameterBuffer(uint32_t frameSetIndex, const shaderio::ReSTIRPTParameters& parameters);
  void UpdateFrameDescriptors(const RenderInput& input);
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

  void FinishFrame(const ResolveHistory::FrameState& frameState);

  // Lifetime dependencies
  // Borrowed from Application through CreateInfo; see ReSTIRPTRendererCreateInfo.

  // Logical device the pipelines and descriptor sets are created on.
  rtpt::VulkanDevice*      m_Device                = nullptr;
  // Allocator for parameter buffers and the resource classes below.
  rtpt::ResourceAllocator* m_GpuResources          = nullptr;
  // Optional debug naming; null disables it.
  const rtpt::Diagnostics* m_Diagnostics           = nullptr;
  // Shared sampling volume must outlive initial sampling and every replay pass.
  const rtpt::SpatiotemporalBlueNoise* m_BlueNoise = nullptr;
  // Frames that can be in flight at once; one descriptor set and parameter buffer each.
  uint32_t                 m_FrameSlotCount        = 0;
  // Size of the bindless texture arrays in the descriptor layout.
  uint32_t                 m_MaxTextureDescriptors = 0;

  // History state

  // Sampling time advances even when camera changes invalidate reuse history.
  uint32_t m_RngFrameNumber = 0;

  // GPU buffers are cleared lazily because the clear must be recorded into a command buffer.
  bool m_NeedsHistoryClear = true;

  // User-facing settings, read at the start of every frame.
  ReSTIRPTSettings m_Settings {};

  // Resources

  // Frame index and surface-buffer parity.
  ReSTIRFrameContext m_FrameContext;
  // Viewport-sized reservoirs, surfaces, and the other per-pass buffers.
  ReSTIRPTResources    m_Resources;
  // Accumulation counter, history signatures, and the NRD denoiser with its input images, shared with the reference path tracer.
  // ReSTIR reuse history is separate and restarts from its FrameState::accumulationRestarted.
  ResolveHistory       m_History;
  // Parameter context is recreated when viewport-dependent reservoir layout changes.
  std::unique_ptr<ReSTIRPTParameterContext> m_ParameterContext;
  // One mapped uniform buffer per frame slot avoids CPU/GPU overwrite hazards.
  std::vector<rtpt::Buffer> m_ParameterBuffers;

  // Pipelines
  // Every pass shares one descriptor layout and push constant range, so the same set binds for ray tracing and compute alike.

  // One descriptor set per frame slot.
  rtpt::DescriptorPack m_DescPack;
  // The layout every pass pipeline is built against.
  VkPipelineLayout     m_PipelineLayout = VK_NULL_HANDLE;
  // Initial sampling traces the path tree, so it is a ray tracing pass; every bounce is traced from its ray generation loop.
  RayTracingPassState m_InitialSamplingPass;
  // Temporal reuse. A ray tracing pass rather than compute, because the hybrid shift traces: one ray per replayed bounce plus one for the reconnection, driven from a loop in ray generation.
  RayTracingPassState m_TemporalPass;
  // Section 3 pre-pass: shifts each pixel's path into its paired partner's domain so both partners can share the result.
  // Ray tracing, not compute: recovering the partner's surface and running the shift both trace.
  RayTracingPassState m_SpatialPrepass;
  // Spatial reuse. Reads a neighbourhood of the array temporal reuse wrote and writes the other array, which holds last frame's history and is dead once temporal has run.
  // Never writing the array it reads is what keeps a pixel from reading a neighbour that another invocation is concurrently rewriting.
  RayTracingPassState m_SpatialPass;
  // Final shading only resolves reservoirs, which needs no rays, so it is a compute pass.
  VkPipeline          m_FinalShadingPipeline = VK_NULL_HANDLE;
  // Section 5's correlation measure. Compute, like final shading: it only reads reservoirs and writes a scalar per pixel, so it needs no rays.
  VkPipeline          m_DuplicationMapPipeline = VK_NULL_HANDLE;
  // Section 6.1 light tile presampling, dispatched before initial sampling reads the tiles.
  VkPipeline          m_LightTilePipeline      = VK_NULL_HANDLE;
  // Section 6.2.2: build the compacted work list.
  VkPipeline          m_PrepassClassifyPipeline = VK_NULL_HANDLE;
  // Section 6.2.2: publish the work list's length as the indirect trace dimensions.
  VkPipeline          m_PrepassOffsetsPipeline  = VK_NULL_HANDLE;

  // Device ray tracing limits. Every pass's recursion depth is checked against them, and every SBT is built against them.
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
};

}  // namespace rtpt
