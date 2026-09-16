#include "PathTracing/ReSTIR/PT/ReSTIRPTRenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>

#include <cmath>
#include <vector>

#include "Framework/Vulkan/Barriers.h"
#include "Framework/Vulkan/Diagnostics.h"

#include "Generated/Shaders/ReSTIRPTDuplicationMap.hlsl.main.h"
#include "Generated/Shaders/ReSTIRPTFinalShading.hlsl.main.h"
#include "Generated/Shaders/ReSTIRPTInitialSampling.hlsl.library.h"
#include "Generated/Shaders/ReSTIRPTLightTiles.hlsl.main.h"
#include "Generated/Shaders/ReSTIRPTPrepassClassify.hlsl.main.h"
#include "Generated/Shaders/ReSTIRPTPrepassOffsets.hlsl.main.h"
#include "Generated/Shaders/ReSTIRPTSpatialPrepass.hlsl.library.h"
#include "Generated/Shaders/ReSTIRPTSpatialResampling.hlsl.library.h"
#include "Generated/Shaders/ReSTIRPTTemporalResampling.hlsl.library.h"

namespace rtpt
{

namespace
{

// Workgroup sizes
// Each constant must match [numthreads] in the shader it dispatches, because the group count is derived from it.

// ReSTIRPTFinalShading.hlsl.
constexpr uint32_t kComputeGroupSize = 8;

// ReSTIRPTLightTiles.hlsl.
constexpr uint32_t kLightTileGroupSize = 8;

// ReSTIRPTDuplicationMap.hlsl. That shader sizes its shared-memory staging window from the tile size, so a mismatch would stage the wrong region rather than merely dispatch inefficiently.
constexpr uint32_t kDuplicationMapGroupSize = 8;

// The Section 6.2.2 full-screen classify pass, ReSTIRPTPrepassClassify.hlsl.
constexpr uint32_t kPrepassSortGroupSize = 8;

// Longest path the settings allow. Initial sampling traces every bounce from a loop in ray generation, so this caps cost rather than reflecting a device limit.
// It must also fit the reconnection-length field, whose range RESTIR_PT_MAX_BOUNCES describes.
constexpr uint32_t kMaxBounces = 8;

static_assert(kMaxBounces <= RESTIR_PT_MAX_BOUNCES);

// Every pass traces its camera, bounce, replay, and shadow rays from ray generation, and their hit shaders only record what was hit, so one level of recursion suffices however long paths get.
constexpr uint32_t kPipelineRecursionDepth = 1;

// Push constant stages
// One push constant range is shared by the ray tracing and compute passes so both can be recorded against the same pipeline layout.

constexpr VkShaderStageFlags kReSTIRPTPushConstantStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;

static_assert(sizeof(shaderio::ReSTIRPTPushConstant) <= 256, "ReSTIR PT push constants must fit Vulkan's minimum 256-byte limit.");

// CmdReSTIRPTMemoryBarrier
// Orders memory writes made in srcStages before reads and writes in dstStages. A memory barrier names no resource, so one call covers every buffer the passes share.

void CmdReSTIRPTMemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStages, VkPipelineStageFlags2 dstStages)
{
  rtpt::CmdMemoryBarrier(cmd, { .stages = srcStages, .access = VK_ACCESS_2_MEMORY_WRITE_BIT }, { .stages = dstStages, .access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT });
}

// Shader code
// SPIR-V compiled by DXC at build time and embedded through the generated headers included above.

std::span<const uint32_t> GetInitialSamplingShaderCode()
{
  return std::span(ReSTIRPTInitialSampling_hlsl);
}

std::span<const uint32_t> GetTemporalShaderCode()
{
  return std::span(ReSTIRPTTemporalResampling_hlsl);
}

std::span<const uint32_t> GetSpatialShaderCode()
{
  return std::span(ReSTIRPTSpatialResampling_hlsl);
}

std::span<const uint32_t> GetSpatialPrepassShaderCode()
{
  return std::span(ReSTIRPTSpatialPrepass_hlsl);
}

std::span<const uint32_t> GetFinalShadingShaderCode()
{
  return std::span(ReSTIRPTFinalShading_hlsl);
}

std::span<const uint32_t> GetDuplicationMapShaderCode()
{
  return std::span(ReSTIRPTDuplicationMap_hlsl);
}

std::span<const uint32_t> GetLightTilesShaderCode()
{
  return std::span(ReSTIRPTLightTiles_hlsl);
}

std::span<const uint32_t> GetPrepassClassifyShaderCode()
{
  return std::span(ReSTIRPTPrepassClassify_hlsl);
}

std::span<const uint32_t> GetPrepassOffsetsShaderCode()
{
  return std::span(ReSTIRPTPrepassOffsets_hlsl);
}

}  // namespace

ReSTIRPTRenderer::ReSTIRPTRenderer(const CreateInfo& createInfo)
    : m_Device(createInfo.device)
    , m_GpuResources(createInfo.resources)
    , m_Diagnostics(createInfo.diagnostics)
    , m_BlueNoise(createInfo.blueNoise)
    , m_FrameSlotCount(createInfo.frameSlotCount)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
    , m_Resources(ReSTIRPTResources::CreateInfo { .resources = createInfo.resources, .diagnostics = createInfo.diagnostics })
    , m_History(ResolveHistory::CreateInfo { .device = createInfo.device, .resources = createInfo.resources, .diagnostics = createInfo.diagnostics, .frameSlotCount = createInfo.frameSlotCount })
{
}

ReSTIRPTRenderer::~ReSTIRPTRenderer() = default;

void ReSTIRPTRenderer::Initialize()
{
  if(m_Device == nullptr || m_GpuResources == nullptr || m_BlueNoise == nullptr || m_FrameSlotCount == 0 || m_MaxTextureDescriptors == 0)
  {
    return;
  }

  // Device objects
  // Vulkan objects are created once; viewport-sized buffers wait until a frame arrives.
  // The ray tracing properties come first so an unusable recursion limit fails before anything is created, and the layouts come before every pipeline built against them.

  QueryRayTracingProperties();
  CreateDescriptorSetLayout();
  CreateParameterBuffers();
  CreatePipelineLayout();
  CreateInitialSamplingPipeline();
  CreateTemporalPipeline();
  CreateSpatialPipeline();
  CreateFinalShadingPipeline();
  m_History.Initialize();
  InvalidateHistory();
}

void ReSTIRPTRenderer::Destroy()
{
  // Without an allocator and a device nothing below was ever created, and every destroy call below needs the device handle.
  if(m_GpuResources == nullptr || m_Device == nullptr)
  {
    return;
  }

  VkDevice device = m_Device->Handle();

  // Dependents
  // Destroy dependents before the descriptor/pipeline layout state they were built against.

  m_History.Destroy();
  m_Resources.Destroy();
  DestroyReSTIRRayTracingPass(device, m_InitialSamplingPass);
  DestroyReSTIRRayTracingPass(device, m_TemporalPass);
  DestroyReSTIRRayTracingPass(device, m_SpatialPass);

  vkDestroyPipeline(device, m_FinalShadingPipeline, nullptr);
  m_FinalShadingPipeline = VK_NULL_HANDLE;
  vkDestroyPipeline(device, m_DuplicationMapPipeline, nullptr);
  m_DuplicationMapPipeline = VK_NULL_HANDLE;
  vkDestroyPipeline(device, m_LightTilePipeline, nullptr);
  m_LightTilePipeline = VK_NULL_HANDLE;
  vkDestroyPipeline(device, m_PrepassClassifyPipeline, nullptr);
  m_PrepassClassifyPipeline = VK_NULL_HANDLE;
  vkDestroyPipeline(device, m_PrepassOffsetsPipeline, nullptr);
  m_PrepassOffsetsPipeline = VK_NULL_HANDLE;
  DestroyReSTIRRayTracingPass(device, m_SpatialPrepass);

  for(rtpt::Buffer& parameterBuffer : m_ParameterBuffers)
  {
    parameterBuffer.Reset();
  }

  m_ParameterBuffers.clear();

  // Layouts
  // Released only after every pipeline and descriptor consumer above is gone.

  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
  m_PipelineLayout = VK_NULL_HANDLE;

  m_DescPack.Destroy();
  m_ParameterContext.reset();

  // CPU state
  // Reset to the same state a fresh renderer starts in, so a later Initialize begins from cleared history.

  m_RngFrameNumber    = 0;
  m_NeedsHistoryClear = true;
}

bool ReSTIRPTRenderer::IsReady() const
{
  return m_PipelineLayout != VK_NULL_HANDLE && IsReSTIRRayTracingPassReady(m_InitialSamplingPass) && IsReSTIRRayTracingPassReady(m_TemporalPass) && IsReSTIRRayTracingPassReady(m_SpatialPass) && IsReSTIRRayTracingPassReady(m_SpatialPrepass) && m_FinalShadingPipeline != VK_NULL_HANDLE && m_DuplicationMapPipeline != VK_NULL_HANDLE && m_LightTilePipeline != VK_NULL_HANDLE && m_PrepassClassifyPipeline != VK_NULL_HANDLE && m_PrepassOffsetsPipeline != VK_NULL_HANDLE;
}

ReSTIRPTSettings& ReSTIRPTRenderer::GetSettings()
{
  return m_Settings;
}

const ReSTIRPTSettings& ReSTIRPTRenderer::GetSettings() const
{
  return m_Settings;
}

uint32_t ReSTIRPTRenderer::GetAccumulatedFrameCount() const
{
  // The counter is only meaningful while an accumulation resolve mode is active.
  return IsAccumulationResolveMode(m_Settings.common.resolveMode) ? m_History.GetAccumulatedFrameCount() : 0;
}

uint32_t ReSTIRPTRenderer::GetBounceLimit() const
{
  return kMaxBounces;
}

VkDeviceSize ReSTIRPTRenderer::GetReservoirMemoryUsage() const
{
  return m_Resources.GetPathReservoirBufferSize();
}

void ReSTIRPTRenderer::InvalidateHistory()
{
  // CPU-side flags reset immediately; GPU buffers are cleared on the next command buffer.

  m_History.InvalidateHistory();
  m_FrameContext.InvalidateHistory();

  m_NeedsHistoryClear = true;

  // Dropping the parameter context resets the reservoir rotation to a known first-frame state, so no pass can read an array the new sequence never wrote.
  m_ParameterContext.reset();
}

rtpt::DescriptorPack& ReSTIRPTRenderer::GetDescriptorPack()
{
  return m_DescPack;
}

const rtpt::DescriptorPack& ReSTIRPTRenderer::GetDescriptorPack() const
{
  return m_DescPack;
}

void ReSTIRPTRenderer::Render(const RenderInput& input)
{
  if(!CanRender(input))
  {
    return;
  }

  const VkExtent2D viewportSize = input.output.extent;

  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  // Frame setup
  // Resources are sized and history decisions are made before anything is written, because the parameter block and descriptors describe those resources. NRD's frame setup is part of those decisions.

  EnsureViewportResources(viewportSize);

  const ResolveHistory::FrameState frameState = BeginFrame(input, viewportSize);

  // Uploads and transitions
  // Everything the passes read - parameters, descriptors, image layouts, cleared history - is settled before the first pass is recorded.

  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(input.frameSlot, m_ParameterBuffers.size());

  UpdateParameterBuffer(frameSetIndex, BuildShaderParameters());
  UpdateFrameDescriptors(input);
  PrepareStorageImages(input, frameState.denoiseEnabled);
  ClearHistoryIfNeeded(input.cmd);

  // Passes
  // The frame index only advances in FinishFrame, after the passes that used it are recorded.
  // NRD reads the accumulation image as its noisy beauty input and composes into the output target.

  RecordPasses(input, BuildPushConstant(input, frameState));

  // Only a frame that denoises runs NRD, so only that frame opens the denoiser scope.
  {
    const GpuProfiler::Zone denoiserZone(frameState.denoiseEnabled ? input.profiler : nullptr, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Denoiser");

    m_History.Denoise(input.cmd, frameState, m_Resources.GetAccumulationImage().descriptor.imageView, input.output.view, m_Settings.common.denoiserDebugView);
  }

  FinishFrame(frameState);
}

bool ReSTIRPTRenderer::CanRender(const RenderInput& input) const
{
  return IsReady() && input.cmd != VK_NULL_HANDLE && input.sceneResource != nullptr && input.sceneInfo != nullptr && input.topLevelAS != nullptr && input.topLevelAS->accel != VK_NULL_HANDLE && input.output;
}

void ReSTIRPTRenderer::EnsureViewportResources(VkExtent2D viewportSize)
{
  // Viewport-sized resources must exist before descriptors point at them.

  m_FrameContext.EnsureViewport(viewportSize);
  m_Resources.EnsureForViewport(viewportSize);
  m_History.EnsureViewportResources(viewportSize);
  EnsureParameterContext(viewportSize);
}

void ReSTIRPTRenderer::EnsureParameterContext(VkExtent2D viewportSize)
{
  // An existing context is kept while the resolution matches, since rebuilding it restarts the reservoir rotation.
  if(m_ParameterContext != nullptr)
  {
    const ReSTIRPTStaticParameters& staticParameters = m_ParameterContext->GetStaticParameters();

    if(staticParameters.renderWidth == viewportSize.width && staticParameters.renderHeight == viewportSize.height)
    {
      return;
    }
  }

  // Reservoir addressing math depends on resolution, so the context is rebuilt with it.
  m_ParameterContext = std::make_unique<ReSTIRPTParameterContext>(ReSTIRPTStaticParameters { .renderWidth = viewportSize.width, .renderHeight = viewportSize.height, });

  m_NeedsHistoryClear = true;
}

ResolveHistory::FrameState ReSTIRPTRenderer::BeginFrame(const RenderInput& input, VkExtent2D viewportSize)
{
  // Frame decisions
  // Every choice that must stay consistent while one frame is recorded is taken once here. The signatures in ResolveHistory tell us when accumulated pixels, or NRD's history, no longer describe the same image.
  // The reference view replaces the beauty image with an unresampled one, so the guide buffers would describe a different render than the signals do. Its signals are reported unavailable: final shading writes none, NRD stands down, and NRD's history is dropped because the reference image comes from a different estimator and NRD would otherwise reproject frames from both.

  const ResolveHistory::FrameState frameState = m_History.BeginFrame(ResolveHistory::FrameInput { .sceneInfo = input.sceneInfo, .topLevelAsAddress = input.topLevelAS->address, .viewportSize = viewportSize, .resolveMode = m_Settings.common.resolveMode, .denoiserSignalsAvailable = !m_Settings.common.referencePathTracer, .denoiserSettings = &m_Settings.common.denoiserSettings, .frameSlot = input.frameSlot, .frameTimeMilliseconds = input.frameTimeMilliseconds });

  // Restarting history also restarts the reservoir rotation, so the parameter context is rebuilt rather than kept. Only an accumulation restart or explicit invalidation does this: a camera move with accumulation off must keep the reservoirs temporal reuse depends on.
  if(frameState.accumulationRestarted)
  {
    m_FrameContext.InvalidateHistory();
    m_ParameterContext.reset();
    EnsureParameterContext(viewportSize);
    m_NeedsHistoryClear = true;
  }

  return frameState;
}

shaderio::ReSTIRPTParameters ReSTIRPTRenderer::BuildShaderParameters()
{
  // Settings may hold any value; the shader loop only ever sees the renderer's fixed maximum.
  m_Settings.initialSampling.maxBounces = std::min(m_Settings.initialSampling.maxBounces, kMaxBounces);

  // Reservoir rotation
  // SetFrameIndex advances the reservoir rotation and must run exactly once per recorded frame, before the other setters. See ReSTIRPTParameterContext.

  m_ParameterContext->SetFrameIndex(m_FrameContext.GetFrameIndex());

  // Parameter blocks
  // The rotation must describe the passes that ACTUALLY run, not the ones the user selected. Claiming a pass that does not exist points shadingInputBufferIndex at an array nothing wrote this frame, which silently shades the previous frame's samples - and a static accumulated image hides that completely, because a one-frame lag averages away.
  // Both reuse passes now exist, so the requested mode is also the effective one.

  m_ParameterContext->SetResamplingMode(m_Settings.common.resamplingMode);
  m_ParameterContext->SetInitialSamplingParameters(m_Settings.initialSampling);
  m_ParameterContext->SetShiftParameters(m_Settings.shift);
  m_ParameterContext->SetTemporalResamplingParameters(m_Settings.temporalResampling);
  m_ParameterContext->SetSpatialResamplingParameters(m_Settings.spatialResampling);
  m_ParameterContext->SetDecorrelationParameters(m_Settings.decorrelation);
  m_ParameterContext->SetShadingParameters(m_Settings.shading);
  m_ParameterContext->SetNeeParameters(m_Settings.nee);

  // Mirror the derived sigma back so the UI reports the value the shaders receive.
  m_Settings.spatialResampling.pairingSigma = m_ParameterContext->GetSpatialResamplingParameters().pairingSigma;

  // Pairing textures
  // Section 3. Textures are rebuilt only when sigma moves, which happens when the user changes the spatial radius.
  // If generation fails, paired reuse is forced off for this frame rather than run against textures that are not involutions.

  std::array<ReSTIRPTPairingTextureParameters, RESTIR_PT_MAX_PAIRING_TEXTURES> pairingTextures {};
  ReSTIRPTSpatialResamplingParameters spatialParameters = m_ParameterContext->GetSpatialResamplingParameters();

  if(spatialParameters.enablePairedSpatialReuse != 0u)
  {
    if(m_Resources.EnsurePairingTextures(spatialParameters.pairingSigma))
    {
      pairingTextures = m_Resources.GetPairingTextureParameters();

      // Per-frame symmetry
      // Re-randomize the pairing every frame. A pairing texture is self-inverting, so left alone it would pair the same two pixels for the lifetime of the render and correlate them permanently - the very failure Section 5 exists to suppress.
      // Conjugating by a symmetry keeps the involution intact while changing which pixels meet.

      uint32_t randomState = m_ParameterContext->GetRuntimeParameters().uniformRandomNumber;

      for(ReSTIRPTPairingTextureParameters& texture : pairingTextures)
      {
        // xorshift32: the per-frame value only has to decorrelate the three slots from each other, not to be a good sampler.
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;

        texture.transform = randomState & 0x7u;
        const uint32_t translationX = (randomState >> 3) % std::max(texture.size, 1u);
        const uint32_t translationY = (randomState >> 15) % std::max(texture.size, 1u);
        texture.translation         = (translationY << 16) | translationX;
      }
    }
    else
    {
      spatialParameters.enablePairedSpatialReuse = 0u;
    }
  }

  // Assembly
  // The spatial block comes from the local copy, so a failed pairing generation reaches the shaders as paired reuse turned off.

  return shaderio::ReSTIRPTParameters {
      .runtimeParams         = m_ParameterContext->GetRuntimeParameters(),
      .reservoirBufferParams = m_ParameterContext->GetReservoirBufferParameters(),
      .bufferIndices         = m_ParameterContext->GetBufferIndices(),
      .initialSampling       = m_ParameterContext->GetInitialSamplingParameters(),
      .shift                 = m_ParameterContext->GetShiftParameters(),
      .temporalResampling    = m_ParameterContext->GetTemporalResamplingParameters(),
      .spatialResampling     = spatialParameters,
      .pairingTextures       = { pairingTextures[0], pairingTextures[1], pairingTextures[2] },
      .decorrelation         = m_ParameterContext->GetDecorrelationParameters(),
      .shading               = m_ParameterContext->GetShadingParameters(),
      .nee                   = m_ParameterContext->GetNeeParameters(),
  };
}

shaderio::ReSTIRPTPushConstant ReSTIRPTRenderer::BuildPushConstant(const RenderInput& input, const ResolveHistory::FrameState& frameState) const
{
  // Flags
  // Per-frame switches travel in the push constant rather than the parameter block.

  uint32_t flags = 0u;

  if(frameState.denoiseEnabled)
  {
    flags |= shaderio::eReSTIRPTFlagWriteDenoiserSignals;
  }

  if(frameState.accumulateEnabled)
  {
    flags |= shaderio::eReSTIRPTFlagAccumulate;
  }

  if(m_Settings.common.referencePathTracer)
  {
    flags |= shaderio::eReSTIRPTFlagReferenceRadiance;
  }

  if(UseSortedPrepass())
  {
    flags |= shaderio::eReSTIRPTFlagSortedPrepass;
  }

  // Push constant

  return shaderio::ReSTIRPTPushConstant {
      .sceneInfoAddress        = (shaderio::GltfSceneInfo*)input.sceneResource->bSceneInfo.address,
      // Advancing every frame keeps the sampler decorrelated even while accumulation is paused or reset.
      .rngFrameNumber          = m_RngFrameNumber,
      .accumulatedFrames       = frameState.accumulateEnabled ? m_History.GetAccumulatedFrameCount() : 0u,
      .maxBounces              = m_Settings.initialSampling.maxBounces,
      .flags                   = flags,
      .reblurHitDistanceParams = { m_Settings.common.denoiserSettings.hitDistanceA, m_Settings.common.denoiserSettings.hitDistanceB, m_Settings.common.denoiserSettings.hitDistanceC },
  };
}

void ReSTIRPTRenderer::PrepareStorageImages(const RenderInput& input, bool denoiserSignalsNeeded)
{
  // Initial sampling and final shading both write storage images.
  TransitionReSTIRStorageImages(input.cmd, const_cast<rtpt::Image&>(m_Resources.GetAccumulationImage()), input.output.image, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  if(!denoiserSignalsNeeded)
  {
    return;
  }

  // NRD guide images
  // The guide buffers are written by final shading, which is a compute pass here, so only the compute stage needs them in a writable layout.
  // Every frame records a barrier, so the write is ordered after the previous frame's writes to the same images.

  m_History.TransitionGuideImagesForWrite(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, StorageImageWriteOrdering::eOrderAfterPreviousWrites);
}

void ReSTIRPTRenderer::ClearHistoryIfNeeded(VkCommandBuffer cmd)
{
  // The clear has to be recorded into a command buffer, so invalidation only sets a flag and the clear happens here.
  if(m_NeedsHistoryClear)
  {
    ClearHistoryBuffers(cmd);
    m_NeedsHistoryClear = false;
  }
}

void ReSTIRPTRenderer::ClearHistoryBuffers(VkCommandBuffer cmd)
{
  // Cleared buffers
  // Zeroed reservoirs read back as M = 0, which every pass treats as "no sample" rather than as a confident black one.
  // One constant sizes both the buffer list and the barrier list below. They used to be written independently, and when a buffer was dropped from the list the barrier array kept its old size: the extra element stayed default-constructed and was submitted with sType 0 and a null VkBuffer.
  // Nothing crashed, because a barrier for no buffer orders nothing - it is only visible with validation layers on.

  constexpr size_t kClearedBufferCount = 5;

  const std::array<const rtpt::Buffer*, kClearedBufferCount> buffers {
      &m_Resources.GetPathReservoirBuffer(),
      &m_Resources.GetSurfaceBuffer(0),
      &m_Resources.GetSurfaceBuffer(1),
      // A stale duplication score would throttle the cap on history that no longer exists, so it is cleared with everything else it describes.
      &m_Resources.GetDuplicationBuffer(),
      // Likewise a motion vector describing a camera pose that no longer applies.
      &m_Resources.GetMotionVectorBuffer(),
  };

  // Order against earlier frames
  // A previous frame may still be executing the passes that write these buffers, and every fill below is a write. Without this the fill races that shader write as a write-after-write hazard: the barrier AFTER the fills orders the fills against later reads, but nothing can retroactively order them against earlier work.
  // Both the source stages and TRANSFER_WRITE on the destination side are spelled out because the inferred masks would produce TRANSFER_READ, which does not cover a fill.

  const VkMemoryBarrier2 clearWriteBarrier {
      .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
      .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
  };

  const VkDependencyInfo clearWriteDependency {
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &clearWriteBarrier,
  };

  vkCmdPipelineBarrier2(cmd, &clearWriteDependency);

  // Fill
  // Each buffer is zeroed and gets a barrier handing it back to the shader stages that read and write history.

  std::array<VkBufferMemoryBarrier2, kClearedBufferCount> barriers {};

  for(size_t i = 0; i < buffers.size(); ++i)
  {
    vkCmdFillBuffer(cmd, buffers[i]->buffer, 0, buffers[i]->bufferSize, 0);

    barriers[i] = VkBufferMemoryBarrier2 {
        .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .buffer        = buffers[i]->buffer,
        .offset        = 0,
        .size          = buffers[i]->bufferSize,
    };
  }

  // Order against later passes
  // Transfer writes must be visible before ray tracing/compute reads them.

  const VkDependencyInfo dependencyInfo {
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = uint32_t(barriers.size()),
      .pBufferMemoryBarriers    = barriers.data(),
  };

  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

// UseSortedPrepass
// Section 6.2.2's compacted pre-pass. Plain on/off: an accumulation-based heuristic was tried and removed, because the divergence it guards against also occurs with a moving camera and accumulation off (see ReSTIRPTSettings.h).

bool ReSTIRPTRenderer::UseSortedPrepass() const
{
  return m_Settings.common.sortPrepass != 0u;
}

void ReSTIRPTRenderer::RecordPasses(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  // Cross-frame ordering
  // Order this frame's first writes against the previous frame's last reads.
  // Every buffer this renderer owns is a single allocation shared by all frames in flight - reservoirs, surfaces, paired shifts, shading weights, motion vectors. Only the descriptor SETS rotate.
  // Submission order does not imply an execution or memory dependency, command buffer boundaries add none, and the frame-slot fence waits for the previous use of its ring slot rather than for the immediately preceding frame - which, with two frames in flight, is not the frame whose reads must complete first.
  // Unconditional on purpose. The only other barrier that could serve is the one after the duplication map, and that is conditional on decorrelation and temporal reuse both being enabled, so it cannot establish this for every mode.
  // It matters most with two reservoir arrays, where initial sampling overwrites the array the previous frame's spatial pass read neighbours from, leaving no slack at all.
  // TRANSFER and DRAW_INDIRECT are in scope as well as the shader stages, because the prepass counter is accessed by neither shader stage alone: it is cleared with vkCmdFillBuffer and then read as indirect trace arguments. Leaving those out let frame N+1's clear race frame N's indirect read - which silently shrinks a dispatch, and so looks like a speedup rather than a fault.

  constexpr VkPipelineStageFlags2 kFrameStages = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;

  CmdReSTIRPTMemoryBarrier(input.cmd, kFrameStages, kFrameStages);

  // Light tiles
  // Section 6.1. Presampling has to complete before any pixel reads a tile, and it depends on nothing this frame produces, so it goes first.

  if(m_Settings.nee.enableLightTiles != 0u)
  {
    RunLightTilePass(input, pushConstant);

    CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  }

  // Initial sampling

  RunInitialSamplingPass(input, pushConstant);

  // Temporal reuse

  if(IsReSTIRPTTemporalResamplingEnabled(m_Settings.common.resamplingMode))
  {
    // Initial sampling writes the candidate reservoirs that temporal reuse reads.
    CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);

    RunTemporalPass(input, pushConstant);
  }

  // Spatial reuse

  if(IsReSTIRPTSpatialResamplingEnabled(m_Settings.common.resamplingMode))
  {
    // Spatial input barrier
    // Spatial reads a neighbourhood of the array the previous pass wrote, so the whole pass must be visible - not just this pixel's own element.
    // COMPUTE is in the destination scope because the sorted pre-pass begins with a COMPUTE dispatch, not a ray-tracing one: its classify pass reads the surfaces and reservoirs the temporal pass writes. Ray tracing alone was correct until a compute pass was put in front of this barrier, and then silently was not.
    // This is a real missing dependency, but be aware it is NOT the cause of the sorted pre-pass divergence documented in ReSTIRPTSettings.h - that survives full ALL_COMMANDS serialization of the whole pass, so it is not a synchronization fault at all. Adding this changed nothing observable.

    CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    // Paired pre-pass
    // Section 3. Each pixel shifts its own path into its partner's domain and publishes the result; resampling then reads both its own record and its partner's instead of tracing either shift.
    // The barrier between is exactly what makes the sharing possible - and is the overhead the paper cites for why the saving is not a full 2x.

    if(m_Settings.spatialResampling.enablePairedSpatialReuse != 0u)
    {
      // Section 6.2.2 compacts the pre-pass into a work list; otherwise every (pixel, slot) is launched.
      if(UseSortedPrepass())
      {
        RunSortedSpatialPrepass(input, pushConstant);
      }
      else
      {
        RunSpatialPrepass(input, pushConstant);
      }

      CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    }

    // Spatial resampling

    RunSpatialPass(input, pushConstant);
  }

  // Final shading
  // The reuse passes write the reservoirs that final shading reads, and the barrier after it leaves the image writes visible to post processing.

  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  RunFinalShadingPass(input, pushConstant);

  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  // Duplication map
  // Section 5's map describes the reservoirs this frame ended with, so it runs last - after every pass that can still change them. It reads the same array final shading just read, and the barrier above already orders it.
  // Only the next frame's temporal pass consumes the result, which is why nothing here waits on it; the frame boundary provides that ordering.
  // Skipped when temporal reuse is off, since nothing would ever read the map.

  if(m_Settings.decorrelation.enable != 0u && IsReSTIRPTTemporalResamplingEnabled(m_Settings.common.resamplingMode))
  {
    RunDuplicationMapPass(input, pushConstant);

    CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  }
}

void ReSTIRPTRenderer::RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const GpuProfiler::Zone zone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Initial sampling");

  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(input.frameSlot, m_DescPack.Sets().size());

  TraceReSTIRRayTracingPass(input.cmd, m_InitialSamplingPass, m_PipelineLayout, *m_DescPack.SetPtr(frameSetIndex), kReSTIRPTPushConstantStages, pushConstant, input.output.extent);
}

void ReSTIRPTRenderer::RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const GpuProfiler::Zone zone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Temporal reuse");

  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(input.frameSlot, m_DescPack.Sets().size());

  TraceReSTIRRayTracingPass(input.cmd, m_TemporalPass, m_PipelineLayout, *m_DescPack.SetPtr(frameSetIndex), kReSTIRPTPushConstantStages, pushConstant, input.output.extent);
}

void ReSTIRPTRenderer::RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const GpuProfiler::Zone zone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Spatial reuse");

  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(input.frameSlot, m_DescPack.Sets().size());

  TraceReSTIRRayTracingPass(input.cmd, m_SpatialPass, m_PipelineLayout, *m_DescPack.SetPtr(frameSetIndex), kReSTIRPTPushConstantStages, pushConstant, input.output.extent);
}

void ReSTIRPTRenderer::RunSortedSpatialPrepass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  // Same scope name as the unsorted pre-pass, so the two variants compare directly; classify and offsets get their own scopes nested inside it.
  const GpuProfiler::Zone zone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Spatial pre-pass");

  const uint32_t      frameSetIndex = GetReSTIRPTFrameSetIndex(input.frameSlot, m_DescPack.Sets().size());
  VkDescriptorSet     descriptorSet = *m_DescPack.SetPtr(frameSetIndex);
  const rtpt::Buffer& counters      = m_Resources.GetPrepassCounterBuffer();
  const VkExtent2D    viewportSize  = input.output.extent;

  // Counter reset
  // The work list count accumulates with InterlockedAdd in the classify pass, so it must start at zero every frame. The indirect dimensions are fully rewritten by the offsets pass, so clearing the whole buffer costs nothing extra.
  // The barrier before the fill is a write-after-read across the FRAME boundary, and it is not covered by the general one at the top of RecordPasses: that one spans ray tracing and compute, while this is a TRANSFER write.
  // This buffer is a single allocation shared by every frame in flight, so without this the fill can land while the previous frame is still reading the counts - which corrupts the launch size, and makes the traced pass read work list entries that were never scattered.

  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
  vkCmdFillBuffer(input.cmd, counters.buffer, 0, counters.bufferSize, 0);
  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  // Classify
  // Append the pairs that need a shift to the work list, and write the paired-shift record for every REJECTED pair - the traced pass never visits those, and a record left untouched would be read next pass as a live shift from an earlier frame.

  {
    const GpuProfiler::Zone classifyZone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Pre-pass classify");

    DispatchReSTIRComputePass(input.cmd, m_PrepassClassifyPipeline, m_PipelineLayout, descriptorSet, kReSTIRPTPushConstantStages, pushConstant, viewportSize, kPrepassSortGroupSize);
  }

  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  // Publish the launch size
  // Copies the count the classify pass accumulated into the indirect trace dimensions. The shader runs one thread, so the dispatch is a single group.

  {
    const GpuProfiler::Zone offsetsZone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Pre-pass offsets");

    vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PrepassOffsetsPipeline);
    vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdPushConstants(input.cmd, m_PipelineLayout, kReSTIRPTPushConstantStages, 0, sizeof(pushConstant), &pushConstant);
    vkCmdDispatch(input.cmd, 1, 1, 1);
  }

  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  // The work list is read by the ray tracing stage and the counter buffer's tail by the indirect-draw stage as launch dimensions, so both dependencies are on this one barrier.
  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);

  // Trace
  // Trace exactly the surviving pairs. The count lives only on the GPU, which is what makes this dispatch indirect.

  vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_SpatialPrepass.pipeline);
  vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
  vkCmdPushConstants(input.cmd, m_PipelineLayout, kReSTIRPTPushConstantStages, 0, sizeof(pushConstant), &pushConstant);

  const rtpt::ShaderBindingTableRegions& regions = m_SpatialPrepass.sbt.Regions();

  // Plain vkCmdTraceRaysIndirectKHR: only the dimensions come from the buffer and the binding table is still supplied here, which is core to VK_KHR_ray_tracing_pipeline - unlike the Indirect2 variant, which also indirects the binding table and needs an extra extension.
  vkCmdTraceRaysIndirectKHR(input.cmd, &regions.raygen, &regions.miss, &regions.hit, &regions.callable, counters.address + VkDeviceSize(RESTIR_PT_PREPASS_INDIRECT_OFFSET) * sizeof(uint32_t));
}

void ReSTIRPTRenderer::RunSpatialPrepass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const GpuProfiler::Zone zone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Spatial pre-pass");

  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(input.frameSlot, m_DescPack.Sets().size());

  TraceReSTIRRayTracingPass(input.cmd, m_SpatialPrepass, m_PipelineLayout, *m_DescPack.SetPtr(frameSetIndex), kReSTIRPTPushConstantStages, pushConstant, input.output.extent);
}

void ReSTIRPTRenderer::RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const GpuProfiler::Zone zone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Final shading");

  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(input.frameSlot, m_DescPack.Sets().size());

  DispatchReSTIRComputePass(input.cmd, m_FinalShadingPipeline, m_PipelineLayout, *m_DescPack.SetPtr(frameSetIndex), kReSTIRPTPushConstantStages, pushConstant, input.output.extent, kComputeGroupSize);
}

void ReSTIRPTRenderer::RunLightTilePass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const GpuProfiler::Zone zone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Light tiles");

  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(input.frameSlot, m_DescPack.Sets().size());

  // The dispatch is shaped by the tile table, not the viewport: one thread per presampled light, x along a tile and y across tiles.
  const VkExtent2D tileExtent { .width = uint32_t(RESTIR_PT_LIGHT_TILE_SIZE), .height = uint32_t(RESTIR_PT_LIGHT_TILE_COUNT) };

  DispatchReSTIRComputePass(input.cmd, m_LightTilePipeline, m_PipelineLayout, *m_DescPack.SetPtr(frameSetIndex), kReSTIRPTPushConstantStages, pushConstant, tileExtent, kLightTileGroupSize);
}

void ReSTIRPTRenderer::RunDuplicationMapPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const GpuProfiler::Zone zone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "ReSTIR PT/Duplication map");

  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(input.frameSlot, m_DescPack.Sets().size());

  // The shader tiles its shared-memory window to an 8x8 group, so the dispatch must use that group size and not the shared compute default.
  DispatchReSTIRComputePass(input.cmd, m_DuplicationMapPipeline, m_PipelineLayout, *m_DescPack.SetPtr(frameSetIndex), kReSTIRPTPushConstantStages, pushConstant, input.output.extent, kDuplicationMapGroupSize);
}

void ReSTIRPTRenderer::FinishFrame(const ResolveHistory::FrameState& frameState)
{
  // History bookkeeping
  // The signatures ResolveHistory records here are what the next BeginFrame compares against, and the frame index only advances for a frame whose passes were recorded.

  m_History.FinishFrame(frameState);
  m_FrameContext.AdvanceFrame();

  ++m_RngFrameNumber;
}

void ReSTIRPTRenderer::QueryRayTracingProperties()
{
  m_RtProperties = m_Device->Support().rayTracingProperties;

  // Every pass pipeline is created with the same fixed recursion depth, so the device only has to support that one.
  if(m_RtProperties.maxRayRecursionDepth < kPipelineRecursionDepth)
  {
    rtpt::CheckVk(VK_ERROR_FEATURE_NOT_PRESENT, "ReSTIRPTRenderer::QueryRayTracingProperties(maxRayRecursionDepth)");
  }
}

void ReSTIRPTRenderer::CreateDescriptorSetLayout()
{
  // Shared layout
  // One descriptor layout is shared by all ReSTIR PT passes so the pass sequence can bind the same set for both the ray tracing and compute stages.

  const VkShaderStageFlags allStages = kReSTIRPTPushConstantStages;

  rtpt::DescriptorBindings bindings;

  // Textures
  // Textures use bindless-style indexing from material records, so the arrays are partially bound and may be updated after binding.

  constexpr VkDescriptorBindingFlags textureFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTTextures, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_MaxTextureDescriptors, allStages, textureFlags);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTHlslTextures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, m_MaxTextureDescriptors, allStages, textureFlags);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTHlslTextureSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, m_MaxTextureDescriptors, allStages, textureFlags);

  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTBlueNoiseTexture, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, allStages);

  // Scene, images, and ReSTIR buffers

  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTOutputImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTAccumulationImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPathReservoirBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);

  // Surface buffers ping-pong for current/previous-frame temporal reuse.
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTCurrentSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPreviousSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);

  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTParamsBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTDuplicationBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPairingBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPairedShiftBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTShadingWeightBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTMotionVectorBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTLightTileBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPrepassWorkBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPrepassCounterBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTDenoiserGuideBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);

  // NRD inputs
  // Bound unconditionally so one descriptor layout serves every resolve mode; final shading writes them only when the denoiser-signal flag is set.

  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTMotionVectorsImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTNormalRoughnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTBaseColorMetalnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTViewZImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTDiffuseRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTSpecularRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.Add(shaderio::ReSTIRPTBindingPoints::eReSTIRPTSpecularDemodulationFactorImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);

  // Descriptor pack
  // Update-after-bind bindings require the set layout and the pool to be created with their update-after-bind flags too. One set is allocated per frame slot.

  rtpt::CheckVk(m_DescPack.Initialize(m_Device->Handle(), bindings, m_FrameSlotCount, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT), "DescriptorPack::Initialize(ReSTIR PT)");
}

void ReSTIRPTRenderer::CreatePipelineLayout()
{
  // Pipeline layout is the ABI between C++ descriptor sets/push constants and HLSL bindings.

  const VkPushConstantRange pushConstantRange {
      .stageFlags = kReSTIRPTPushConstantStages,
      .offset     = 0,
      .size       = sizeof(shaderio::ReSTIRPTPushConstant),
  };

  const VkPipelineLayoutCreateInfo pipelineLayoutInfo {
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = m_DescPack.LayoutPtr(),
      .pushConstantRangeCount = 1,
      .pPushConstantRanges    = &pushConstantRange,
  };

  rtpt::CheckVk(vkCreatePipelineLayout(m_Device->Handle(), &pipelineLayoutInfo, nullptr, &m_PipelineLayout), "vkCreatePipelineLayout(ReSTIR PT)");
}

void ReSTIRPTRenderer::CreateParameterBuffers()
{
  const uint32_t frameSetCount = m_FrameSlotCount;

  m_ParameterBuffers.resize(frameSetCount);

  for(rtpt::Buffer& parameterBuffer : m_ParameterBuffers)
  {
    // Mapped uniform buffers are updated once per frame set before recording passes.
    rtpt::CheckVk(m_GpuResources->CreateBuffer(parameterBuffer, sizeof(shaderio::ReSTIRPTParameters), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT), "ResourceAllocator::CreateBuffer(ReSTIR parameters)");
  }
}

void ReSTIRPTRenderer::CreateInitialSamplingPipeline()
{
  // Initial sampling traces every bounce from its ray generation loop, so its recursion depth stays fixed however long paths get.
  CreateReSTIRRayTracingPass(*m_GpuResources, m_Diagnostics, m_RtProperties, m_PipelineLayout, GetInitialSamplingShaderCode(), kPipelineRecursionDepth, "ReSTIR PT Initial Sampling Pipeline", m_InitialSamplingPass);
}

void ReSTIRPTRenderer::CreateTemporalPipeline()
{
  // Replay traces one ray per regenerated bounce from the ray generation loop, so the recursion budget stays fixed regardless of path length.
  CreateReSTIRRayTracingPass(*m_GpuResources, m_Diagnostics, m_RtProperties, m_PipelineLayout, GetTemporalShaderCode(), kPipelineRecursionDepth, "ReSTIR PT Temporal Resampling Pipeline", m_TemporalPass);
}

void ReSTIRPTRenderer::CreateSpatialPipeline()
{
  // The spatial pass and its paired pre-pass trace only from their ray generation loops, exactly as temporal reuse does, so they share its recursion budget.
  CreateReSTIRRayTracingPass(*m_GpuResources, m_Diagnostics, m_RtProperties, m_PipelineLayout, GetSpatialShaderCode(), kPipelineRecursionDepth, "ReSTIR PT Spatial Resampling Pipeline", m_SpatialPass);
  CreateReSTIRRayTracingPass(*m_GpuResources, m_Diagnostics, m_RtProperties, m_PipelineLayout, GetSpatialPrepassShaderCode(), kPipelineRecursionDepth, "ReSTIR PT Spatial Prepass Pipeline", m_SpatialPrepass);
}

void ReSTIRPTRenderer::CreateFinalShadingPipeline()
{
  // Compute pipelines
  // Creates every compute pass, not only final shading: none of them trace rays, and all share the one pipeline layout.

  m_FinalShadingPipeline    = CreateReSTIRComputePipeline(m_Device->Handle(), m_Diagnostics, m_PipelineLayout, GetFinalShadingShaderCode(), "ReSTIR PT Final Shading Pipeline");
  m_DuplicationMapPipeline  = CreateReSTIRComputePipeline(m_Device->Handle(), m_Diagnostics, m_PipelineLayout, GetDuplicationMapShaderCode(), "ReSTIR PT Duplication Map Pipeline");
  m_LightTilePipeline       = CreateReSTIRComputePipeline(m_Device->Handle(), m_Diagnostics, m_PipelineLayout, GetLightTilesShaderCode(), "ReSTIR PT Light Tiles Pipeline");
  m_PrepassClassifyPipeline = CreateReSTIRComputePipeline(m_Device->Handle(), m_Diagnostics, m_PipelineLayout, GetPrepassClassifyShaderCode(), "ReSTIR PT Prepass Classify Pipeline");
  m_PrepassOffsetsPipeline  = CreateReSTIRComputePipeline(m_Device->Handle(), m_Diagnostics, m_PipelineLayout, GetPrepassOffsetsShaderCode(), "ReSTIR PT Prepass Offsets Pipeline");
}

void ReSTIRPTRenderer::UpdateFrameDescriptors(const RenderInput& input)
{
  // Frame set and history
  // Each frame slot has its own set, and the surface buffers swap current/previous roles by frame parity.

  const uint32_t frameSetIndex        = GetReSTIRPTFrameSetIndex(input.frameSlot, m_DescPack.Sets().size());
  const uint32_t currentHistoryIndex  = m_FrameContext.GetCurrentHistoryIndex();
  const uint32_t previousHistoryIndex = m_FrameContext.GetPreviousHistoryIndex();

  // Image descriptors
  // Storage images are bound in GENERAL, the layout PrepareStorageImages transitions them to before any pass runs.
  // NRD guide images are always bound; final shading only writes them when the denoiser-signal flag is set.

  VkDescriptorImageInfo outputImageInfo = input.output.Descriptor(VK_IMAGE_LAYOUT_GENERAL);
  outputImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorImageInfo accumulationImageInfo = m_Resources.GetAccumulationImage().descriptor;
  accumulationImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorImageInfo motionVectorsImageInfo = m_History.GetDenoiserResources().GetMotionVectorsImage().descriptor;
  motionVectorsImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorImageInfo normalRoughnessImageInfo = m_History.GetDenoiserResources().GetNormalRoughnessImage().descriptor;
  normalRoughnessImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorImageInfo baseColorMetalnessImageInfo = m_History.GetDenoiserResources().GetBaseColorMetalnessImage().descriptor;
  baseColorMetalnessImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorImageInfo viewZImageInfo = m_History.GetDenoiserResources().GetViewZImage().descriptor;
  viewZImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorImageInfo diffuseRadianceHitDistanceImageInfo = m_History.GetDenoiserResources().GetDiffuseRadianceHitDistanceImage().descriptor;
  diffuseRadianceHitDistanceImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorImageInfo specularRadianceHitDistanceImageInfo = m_History.GetDenoiserResources().GetSpecularRadianceHitDistanceImage().descriptor;
  specularRadianceHitDistanceImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorImageInfo specularDemodulationFactorImageInfo = m_History.GetDenoiserResources().GetSpecularDemodulationFactorImage().descriptor;
  specularDemodulationFactorImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  // Buffer descriptors
  // The order here must match bufferBindings below; the two arrays are paired by index.

  const std::array<VkDescriptorBufferInfo, 13> bufferInfos {
      // One buffer holds every rotating reservoir array.
      VkDescriptorBufferInfo { m_Resources.GetPathReservoirBuffer().buffer, 0, VK_WHOLE_SIZE },
      // Surface buffers are bound as current/previous according to frame parity.
      VkDescriptorBufferInfo { m_Resources.GetSurfaceBuffer(currentHistoryIndex).buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_Resources.GetSurfaceBuffer(previousHistoryIndex).buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_ParameterBuffers[frameSetIndex].buffer, 0, sizeof(shaderio::ReSTIRPTParameters) },
      VkDescriptorBufferInfo { m_Resources.GetDuplicationBuffer().buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_Resources.GetPairingBuffer().buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_Resources.GetPairedShiftBuffer().buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_Resources.GetShadingWeightBuffer().buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_Resources.GetMotionVectorBuffer().buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_Resources.GetLightTileBuffer().buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_Resources.GetPrepassWorkBuffer().buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_Resources.GetPrepassCounterBuffer().buffer, 0, VK_WHOLE_SIZE },
      VkDescriptorBufferInfo { m_Resources.GetDenoiserGuideBuffer().buffer, 0, VK_WHOLE_SIZE },
  };

  // Acceleration structure
  // TLAS descriptors attach through pNext rather than pBufferInfo/pImageInfo.

  VkAccelerationStructureKHR accel = input.topLevelAS->accel;

  const VkWriteDescriptorSetAccelerationStructureKHR accelerationInfo {
      .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
      .accelerationStructureCount = 1,
      .pAccelerationStructures    = &accel,
  };

  // Writes
  // Every write is collected into one array and submitted in a single vkUpdateDescriptorSets call.

  const VkDescriptorImageInfo blueNoiseInfo = m_BlueNoise->Descriptor();

  std::array<VkWriteDescriptorSet, 25> writes {};
  uint32_t                             writeCount = 0;

  writes[writeCount]       = m_DescPack.MakeWrite(shaderio::ReSTIRPTBindingPoints::eReSTIRPTTlas, frameSetIndex);
  writes[writeCount].pNext = &accelerationInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.MakeWrite(shaderio::ReSTIRPTBindingPoints::eReSTIRPTOutputImage, frameSetIndex);
  writes[writeCount].pImageInfo = &outputImageInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.MakeWrite(shaderio::ReSTIRPTBindingPoints::eReSTIRPTAccumulationImage, frameSetIndex);
  writes[writeCount].pImageInfo = &accumulationImageInfo;
  ++writeCount;

  const std::array<std::pair<uint32_t, const VkDescriptorImageInfo*>, 7> denoiserImageBindings { {
      { shaderio::ReSTIRPTBindingPoints::eReSTIRPTMotionVectorsImage, &motionVectorsImageInfo },
      { shaderio::ReSTIRPTBindingPoints::eReSTIRPTNormalRoughnessImage, &normalRoughnessImageInfo },
      { shaderio::ReSTIRPTBindingPoints::eReSTIRPTBaseColorMetalnessImage, &baseColorMetalnessImageInfo },
      { shaderio::ReSTIRPTBindingPoints::eReSTIRPTViewZImage, &viewZImageInfo },
      { shaderio::ReSTIRPTBindingPoints::eReSTIRPTDiffuseRadianceHitDistanceImage, &diffuseRadianceHitDistanceImageInfo },
      { shaderio::ReSTIRPTBindingPoints::eReSTIRPTSpecularRadianceHitDistanceImage, &specularRadianceHitDistanceImageInfo },
      { shaderio::ReSTIRPTBindingPoints::eReSTIRPTSpecularDemodulationFactorImage, &specularDemodulationFactorImageInfo },
  } };

  for(const auto& [binding, imageInfo] : denoiserImageBindings)
  {
    writes[writeCount]            = m_DescPack.MakeWrite(binding, frameSetIndex);
    writes[writeCount].pImageInfo = imageInfo;
    ++writeCount;
  }

  const std::array<uint32_t, 13> bufferBindings {
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTPathReservoirBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTCurrentSurfaceBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTPreviousSurfaceBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTParamsBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTDuplicationBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTPairingBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTPairedShiftBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTShadingWeightBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTMotionVectorBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTLightTileBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTPrepassWorkBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTPrepassCounterBuffer,
      shaderio::ReSTIRPTBindingPoints::eReSTIRPTDenoiserGuideBuffer,
  };

  for(size_t i = 0; i < bufferBindings.size(); ++i)
  {
    writes[writeCount]             = m_DescPack.MakeWrite(bufferBindings[i], frameSetIndex);
    writes[writeCount].pBufferInfo = &bufferInfos[i];
    ++writeCount;
  }

  writes[writeCount]            = m_DescPack.MakeWrite(shaderio::ReSTIRPTBindingPoints::eReSTIRPTBlueNoiseTexture, frameSetIndex);
  writes[writeCount].pImageInfo = &blueNoiseInfo;
  ++writeCount;

  vkUpdateDescriptorSets(m_Device->Handle(), writeCount, writes.data(), 0, nullptr);
}

void ReSTIRPTRenderer::UpdateParameterBuffer(uint32_t frameSetIndex, const shaderio::ReSTIRPTParameters& parameters)
{
  if(frameSetIndex >= m_ParameterBuffers.size())
  {
    return;
  }

  // Host writes to the mapped uniform buffer are flushed before recording uses it.

  rtpt::Buffer& parameterBuffer = m_ParameterBuffers[frameSetIndex];

  std::memcpy(parameterBuffer.mapping, &parameters, sizeof(parameters));
  rtpt::CheckVk(m_GpuResources->FlushBuffer(parameterBuffer, 0, sizeof(parameters)), "ResourceAllocator::FlushBuffer(ReSTIR parameters)");
}

}  // namespace rtpt
