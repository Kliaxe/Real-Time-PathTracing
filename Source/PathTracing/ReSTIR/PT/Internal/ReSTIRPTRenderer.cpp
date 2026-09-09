#include "PathTracing/ReSTIR/PT/ReSTIRPTRenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>

#include <cmath>
#include <vector>

#include <nvapp/application.hpp>
#include <nvutils/logger.hpp>
#include <nvvk/barriers.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

#include "Common/Utils.hpp"
#include "Denoising/DenoiserResources.h"
#include "Denoising/NrdDenoiser.h"

#include "_autogen/ReSTIRPTDuplicationMap.slang.h"
#include "_autogen/ReSTIRPTLightTiles.slang.h"
#include "_autogen/ReSTIRPTPrepassClassify.slang.h"
#include "_autogen/ReSTIRPTPrepassOffsets.slang.h"
#include "_autogen/ReSTIRPTFinalShading.slang.h"
#include "_autogen/ReSTIRPTInitialSampling.slang.h"
#include "_autogen/ReSTIRPTSpatialPrepass.slang.h"
#include "_autogen/ReSTIRPTSpatialResampling.slang.h"
#include "_autogen/ReSTIRPTTemporalResampling.slang.h"

namespace nvsamples
{

namespace
{

constexpr uint32_t kComputeGroupSize    = 8;
// Must match [numthreads] in ReSTIRPTDuplicationMap.slang: that shader sizes its
// shared-memory staging window from the tile size, so a mismatch would stage the
// wrong region rather than merely dispatch inefficiently.
// Must match [numthreads] in ReSTIRPTLightTiles.slang.
constexpr uint32_t kLightTileGroupSize = 8;
constexpr uint32_t kDuplicationMapGroupSize = 8;
// Must match [numthreads] in the two Section 6.2.2 full-screen sorting passes.
constexpr uint32_t kPrepassSortGroupSize = 8;
constexpr uint32_t kRequestedMaxBounces = 8;

// One push constant range is shared by the ray tracing and compute passes so both
// can be recorded against the same pipeline layout.
constexpr VkShaderStageFlags kReSTIRPTPushConstantStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR
                                                           | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                                                           | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;
static_assert(sizeof(shaderio::ReSTIRPTPushConstant) <= 256, "ReSTIR PT push constants must fit Vulkan's minimum 256-byte limit.");

// Memory barrier with the acceleration-structure access bit left off.
//
// nvvk::cmdMemoryBarrier infers its access masks from the stage masks, and any
// destination stage set containing RAY_TRACING_SHADER unconditionally picks up
// ACCELERATION_STRUCTURE_READ. Vulkan forbids that bit when the destination also
// names another shader stage, unless the rayQuery feature is enabled - and this
// renderer never issues an inline query, since the hybrid shift needs the payload
// that only a pipeline trace carries, so the feature is not requested.
//
// Dropping the bit is not merely a workaround for the validation message: none of
// these barriers guard the acceleration structure. They order reservoir, surface,
// pairing and work-list memory between passes. The TLAS is built and synchronized
// by the scene runtime before any of this is recorded.
//
// Everything else is nvpro's own inference, so the ordering these barriers
// establish is unchanged.
constexpr VkAccessFlags2 InferAccessWithoutAccelerationStructure(VkPipelineStageFlags2 stages, bool read)
{
  return nvvk::inferAccessMaskFromStage(stages, read) & ~VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
}

void CmdReSTIRPTMemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStages, VkPipelineStageFlags2 dstStages)
{
  nvvk::cmdMemoryBarrier(cmd, srcStages, dstStages, InferAccessWithoutAccelerationStructure(srcStages, false),
                         InferAccessWithoutAccelerationStructure(dstStages, true));
}

VkShaderModuleCreateInfo GetInitialSamplingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTInitialSampling_slang));
}

VkShaderModuleCreateInfo GetTemporalShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTTemporalResampling_slang));
}

VkShaderModuleCreateInfo GetSpatialShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTSpatialResampling_slang));
}

VkShaderModuleCreateInfo GetSpatialPrepassShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTSpatialPrepass_slang));
}

VkShaderModuleCreateInfo GetFinalShadingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTFinalShading_slang));
}

VkShaderModuleCreateInfo GetDuplicationMapShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTDuplicationMap_slang));
}

VkShaderModuleCreateInfo GetLightTilesShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTLightTiles_slang));
}

VkShaderModuleCreateInfo GetPrepassClassifyShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTPrepassClassify_slang));
}

VkShaderModuleCreateInfo GetPrepassOffsetsShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTPrepassOffsets_slang));
}

}  // namespace

ReSTIRPTRenderer::ReSTIRPTRenderer(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
    , m_Resources(ReSTIRPTResources::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
    , m_DenoiserResources(DenoiserResources::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
    , m_NrdDenoiser(NrdDenoiser::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
{
}

ReSTIRPTRenderer::~ReSTIRPTRenderer() = default;

void ReSTIRPTRenderer::Initialize()
{
  if(m_App == nullptr || m_Allocator == nullptr || m_MaxTextureDescriptors == 0)
  {
    return;
  }

  // Vulkan objects are created once; viewport-sized buffers wait until a frame arrives.
  QueryRayTracingProperties();
  CreateDescriptorSetLayout();
  CreateParameterBuffers();
  CreatePipelineLayout();
  CreateInitialSamplingPipeline();
  CreateTemporalPipeline();
  CreateSpatialPipeline();
  CreateFinalShadingPipeline();
  m_NrdDenoiser.Initialize();
  InvalidateHistory();
}

void ReSTIRPTRenderer::Destroy()
{
  if(m_Allocator == nullptr)
  {
    return;
  }

  VkDevice device = m_Allocator->getDevice();

  // Destroy dependents before the descriptor/pipeline layout state they were built against.
  m_NrdDenoiser.Destroy();
  m_DenoiserResources.Destroy();
  m_Resources.Destroy();
  DestroyReSTIRRayTracingPass(m_Allocator, m_InitialSamplingPass);
  DestroyReSTIRRayTracingPass(m_Allocator, m_TemporalPass);
  DestroyReSTIRRayTracingPass(m_Allocator, m_SpatialPass);

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
  DestroyReSTIRRayTracingPass(m_Allocator, m_SpatialPrepass);

  for(nvvk::Buffer& parameterBuffer : m_ParameterBuffers)
  {
    m_Allocator->destroyBuffer(parameterBuffer);
    parameterBuffer = {};
  }
  m_ParameterBuffers.clear();

  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
  m_PipelineLayout = VK_NULL_HANDLE;

  m_DescPack.deinit();
  m_ParameterContext.reset();
  m_AccumulatedFrames        = 0;
  m_HistoryInvalidated       = true;
  m_HasAccumulationSignature = false;
  m_HasDenoiserSignature     = false;
  m_NeedsHistoryClear        = true;
}

bool ReSTIRPTRenderer::IsReady() const
{
  return m_PipelineLayout != VK_NULL_HANDLE && IsReSTIRRayTracingPassReady(m_InitialSamplingPass)
         && IsReSTIRRayTracingPassReady(m_TemporalPass) && IsReSTIRRayTracingPassReady(m_SpatialPass)
         && m_FinalShadingPipeline != VK_NULL_HANDLE && m_DuplicationMapPipeline != VK_NULL_HANDLE
         && m_LightTilePipeline != VK_NULL_HANDLE;
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
  return IsAccumulationResolveMode(m_Settings.common.resolveMode) ? m_AccumulatedFrames : 0;
}

uint32_t ReSTIRPTRenderer::GetPipelineBounceLimit() const
{
  return m_PipelineBounceLimit;
}

VkDeviceSize ReSTIRPTRenderer::GetReservoirMemoryUsage() const
{
  return m_Resources.GetPathReservoirBufferSize();
}

void ReSTIRPTRenderer::InvalidateHistory()
{
  // CPU-side flags reset immediately; GPU buffers are cleared on the next command buffer.
  m_AccumulatedFrames        = 0;
  m_HistoryInvalidated       = true;
  m_HasAccumulationSignature = false;
  m_HasDenoiserSignature     = false;
  m_NeedsHistoryClear        = true;
  m_FrameContext.InvalidateHistory();
  m_NrdDenoiser.InvalidateHistory();
  // Dropping the parameter context resets the reservoir rotation to a known
  // first-frame state, so no pass can read an array the new sequence never wrote.
  m_ParameterContext.reset();
}

nvvk::DescriptorPack& ReSTIRPTRenderer::GetDescriptorPack()
{
  return m_DescPack;
}

const nvvk::DescriptorPack& ReSTIRPTRenderer::GetDescriptorPack() const
{
  return m_DescPack;
}

void ReSTIRPTRenderer::Render(const RenderInput& input)
{
  if(!CanRender(input))
  {
    return;
  }

  const VkExtent2D viewportSize = input.gBuffers->getSize();
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  EnsureViewportResources(viewportSize);
  const FrameState frameState = BeginFrame(input, viewportSize);

  // The descriptor set index follows nvpro's frame cycle to avoid overwriting in-flight data.
  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_ParameterBuffers.size());
  UpdateParameterBuffer(frameSetIndex, BuildShaderParameters());
  UpdateFrameDescriptors(input);
  PrepareDenoiser(input, frameState);
  PrepareStorageImages(input, frameState.denoiserSignalsNeeded);
  ClearHistoryIfNeeded(input.cmd);

  RecordPasses(input, BuildPushConstant(input, frameState.denoiserSignalsNeeded));
  RunDenoiserIfNeeded(input, frameState);
  FinishFrame(frameState);
}

bool ReSTIRPTRenderer::CanRender(const RenderInput& input) const
{
  return IsReady() && input.cmd != VK_NULL_HANDLE && input.sceneResource != nullptr && input.sceneInfo != nullptr
         && input.topLevelAS != nullptr && input.topLevelAS->accel != VK_NULL_HANDLE && input.gBuffers != nullptr;
}

void ReSTIRPTRenderer::EnsureViewportResources(VkExtent2D viewportSize)
{
  // Viewport-sized resources must exist before descriptors point at them.
  m_FrameContext.EnsureViewport(viewportSize);
  m_Resources.EnsureForViewport(viewportSize);
  m_DenoiserResources.EnsureForViewport(viewportSize);
  EnsureParameterContext(viewportSize);
}

void ReSTIRPTRenderer::EnsureParameterContext(VkExtent2D viewportSize)
{
  if(m_ParameterContext != nullptr)
  {
    const ReSTIRPTStaticParameters& staticParameters = m_ParameterContext->GetStaticParameters();
    if(staticParameters.renderWidth == viewportSize.width && staticParameters.renderHeight == viewportSize.height)
    {
      return;
    }
  }

  // Reservoir addressing math depends on resolution, so the context is rebuilt with it.
  m_ParameterContext = std::make_unique<ReSTIRPTParameterContext>(ReSTIRPTStaticParameters{
      .renderWidth  = viewportSize.width,
      .renderHeight = viewportSize.height,
  });
  m_NeedsHistoryClear = true;
}

ReSTIRPTRenderer::FrameState ReSTIRPTRenderer::BeginFrame(const RenderInput& input, VkExtent2D viewportSize)
{
  FrameState frameState{};
  frameState.viewportSize          = viewportSize;
  frameState.referenceRadianceActive = m_Settings.common.referencePathTracer;
  frameState.accumulationSignature = MakeReSTIRPTAccumulationSignature(*input.sceneInfo, input.topLevelAS->address, viewportSize);
  frameState.denoiseEnabled        = IsDenoiseResolveMode(m_Settings.common.resolveMode);
  // The reference view replaces the beauty image with an unresampled one, so the
  // guide buffers would describe a different render than the signals do.
  frameState.denoiserSignalsNeeded = frameState.denoiseEnabled && !frameState.referenceRadianceActive;
  frameState.denoiserSignature = MakeReSTIRPTDenoiserHistorySignature(*input.sceneInfo, input.topLevelAS->address, viewportSize);

  // The signature tells us when accumulated pixels no longer describe the same image.
  const bool accumulationSignatureChanged =
      !m_HasAccumulationSignature
      || std::memcmp(&frameState.accumulationSignature, &m_LastAccumulationSignature, sizeof(AccumulationSignature)) != 0;
  const bool historyInvalidated =
      m_HistoryInvalidated || (IsAccumulationResolveMode(m_Settings.common.resolveMode) && accumulationSignatureChanged);

  const bool denoiserSignatureChanged =
      !m_HasDenoiserSignature
      || std::memcmp(&frameState.denoiserSignature, &m_LastDenoiserSignature, sizeof(DenoiserSignature)) != 0;
  frameState.denoiserHistoryInvalidated =
      m_HistoryInvalidated || (frameState.denoiseEnabled && denoiserSignatureChanged);

  if(historyInvalidated)
  {
    m_AccumulatedFrames = 0;
    m_FrameContext.InvalidateHistory();
    m_ParameterContext.reset();
    EnsureParameterContext(viewportSize);
    m_NeedsHistoryClear = true;
  }

  return frameState;
}

shaderio::ReSTIRPTParameters ReSTIRPTRenderer::BuildShaderParameters()
{
  // The UI value cannot exceed the recursion depth this device's pipeline supports.
  m_Settings.initialSampling.maxBounces = std::min(m_Settings.initialSampling.maxBounces, m_PipelineBounceLimit);

  // SetFrameIndex advances the reservoir rotation and must run exactly once per
  // recorded frame, before the other setters. See ReSTIRPTParameterContext.
  m_ParameterContext->SetFrameIndex(m_FrameContext.GetFrameIndex());

  // The rotation must describe the passes that ACTUALLY run, not the ones the user
  // selected. Claiming a pass that does not exist points shadingInputBufferIndex at
  // an array nothing wrote this frame, which silently shades the previous frame's
  // samples - and a static accumulated image hides that completely, because a
  // one-frame lag averages away. Both reuse passes now exist, so the requested
  // mode is also the effective one.
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

  // Section 3. Textures are rebuilt only when sigma moves, which happens when the
  // user changes the spatial radius. If generation fails, paired reuse is forced
  // off for this frame rather than run against textures that are not involutions.
  std::array<ReSTIRPTPairingTextureParameters, RESTIR_PT_MAX_PAIRING_TEXTURES> pairingTextures{};
  ReSTIRPTSpatialResamplingParameters spatialParameters = m_ParameterContext->GetSpatialResamplingParameters();
  if(spatialParameters.enablePairedSpatialReuse != 0u)
  {
    if(m_Resources.EnsurePairingTextures(spatialParameters.pairingSigma))
    {
      pairingTextures = m_Resources.GetPairingTextureParameters();

      // Re-randomize the pairing every frame. A pairing texture is self-inverting,
      // so left alone it would pair the same two pixels for the lifetime of the
      // render and correlate them permanently - the very failure Section 5 exists
      // to suppress. Conjugating by a symmetry keeps the involution intact while
      // changing which pixels meet.
      uint32_t randomState = m_ParameterContext->GetRuntimeParameters().uniformRandomNumber;
      for(ReSTIRPTPairingTextureParameters& texture : pairingTextures)
      {
        // xorshift32: the per-frame value only has to decorrelate the three slots
        // from each other, not to be a good sampler.
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

  return shaderio::ReSTIRPTParameters{
      .runtimeParams         = m_ParameterContext->GetRuntimeParameters(),
      .reservoirBufferParams = m_ParameterContext->GetReservoirBufferParameters(),
      .bufferIndices         = m_ParameterContext->GetBufferIndices(),
      .initialSampling       = m_ParameterContext->GetInitialSamplingParameters(),
      .shift                 = m_ParameterContext->GetShiftParameters(),
      .temporalResampling    = m_ParameterContext->GetTemporalResamplingParameters(),
      .spatialResampling     = spatialParameters,
      .pairingTextures       = {pairingTextures[0], pairingTextures[1], pairingTextures[2]},
      .decorrelation         = m_ParameterContext->GetDecorrelationParameters(),
      .shading               = m_ParameterContext->GetShadingParameters(),
      .nee                   = m_ParameterContext->GetNeeParameters(),
  };
}

shaderio::ReSTIRPTPushConstant ReSTIRPTRenderer::BuildPushConstant(const RenderInput& input, bool denoiserSignalsNeeded) const
{
  uint32_t flags = 0u;
  if(denoiserSignalsNeeded)
  {
    flags |= shaderio::eReSTIRPTFlagWriteDenoiserSignals;
  }
  if(IsAccumulationResolveMode(m_Settings.common.resolveMode))
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

  return shaderio::ReSTIRPTPushConstant{
      .sceneInfoAddress  = (shaderio::GltfSceneInfo*)input.sceneResource->bSceneInfo.address,
      // Advancing every frame keeps the sampler decorrelated even while
      // accumulation is paused or reset.
      .rngFrameNumber    = m_FrameContext.GetFrameIndex(),
      .accumulatedFrames = IsAccumulationResolveMode(m_Settings.common.resolveMode) ? m_AccumulatedFrames : 0u,
      .maxBounces        = m_Settings.initialSampling.maxBounces,
      .flags             = flags,
      .reblurHitDistanceParams = {m_Settings.common.denoiserSettings.hitDistanceA,
                                  m_Settings.common.denoiserSettings.hitDistanceB,
                                  m_Settings.common.denoiserSettings.hitDistanceC},
  };
}

void ReSTIRPTRenderer::PrepareDenoiser(const RenderInput& input, const FrameState& frameState)
{
  if(!frameState.denoiseEnabled)
  {
    return;
  }

  // NRD needs its camera history and guide-buffer descriptors settled before the
  // passes that write into them are recorded.
  m_NrdDenoiser.PrepareFrame(NrdDenoiser::FrameInput{
                                 .sceneInfo          = input.sceneInfo,
                                 .viewportSize       = frameState.viewportSize,
                                 .historyInvalidated = frameState.denoiserHistoryInvalidated,
                                 // Final shading demodulates both signals, so the
                                 // compose pass has to put the material back.
                                 .enableMaterialDemodulation = true,
                                 .settings                   = &m_Settings.common.denoiserSettings,
                             },
                             m_DenoiserResources);
}

void ReSTIRPTRenderer::RunDenoiserIfNeeded(const RenderInput& input, const FrameState& frameState)
{
  if(!frameState.denoiseEnabled)
  {
    return;
  }

  if(frameState.referenceRadianceActive)
  {
    // The reference image comes from a different estimator, so NRD history must not
    // continue across the switch: it would reproject frames from both.
    m_NrdDenoiser.InvalidateHistory();
    return;
  }

  if(m_NrdDenoiser.IsReady())
  {
    m_NrdDenoiser.Denoise(input.cmd, m_DenoiserResources, m_Resources.GetAccumulationImage().descriptor.imageView,
                          input.gBuffers->getColorImageView(input.renderedImageIndex), m_Settings.common.denoiserDebugView,
                          frameState.viewportSize);
  }
}

void ReSTIRPTRenderer::PrepareStorageImages(const RenderInput& input, bool denoiserSignalsNeeded)
{
  // Initial sampling and final shading both write storage images.
  TransitionReSTIRStorageImages(input.cmd, const_cast<nvvk::Image&>(m_Resources.GetAccumulationImage()),
                                  input.gBuffers->getColorImage(input.renderedImageIndex),
                                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  if(!denoiserSignalsNeeded)
  {
    return;
  }

  // The guide buffers are written by final shading, which is a compute pass here
  // rather than the ray tracing pass ReSTIR DI uses.
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetMotionVectorsImage(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetNormalRoughnessImage(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetBaseColorMetalnessImage(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetViewZImage(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetDiffuseRadianceHitDistanceImage(),
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetSpecularRadianceHitDistanceImage(),
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetSpecularDemodulationFactorImage(),
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void ReSTIRPTRenderer::ClearHistoryIfNeeded(VkCommandBuffer cmd)
{
  if(m_NeedsHistoryClear)
  {
    ClearHistoryBuffers(cmd);
    m_NeedsHistoryClear = false;
  }
}

void ReSTIRPTRenderer::ClearHistoryBuffers(VkCommandBuffer cmd)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(cmd, "ReSTIR PT Clear History");

  // Zeroed reservoirs read back as M = 0, which every pass treats as "no sample"
  // rather than as a confident black one.
  //
  // One constant sizes both the buffer list and the barrier list below. They used to
  // be written independently, and when a buffer was dropped from the list the barrier
  // array kept its old size: the extra element stayed default-constructed and was
  // submitted with sType 0 and a null VkBuffer. Nothing crashed, because a barrier
  // for no buffer orders nothing - it is only visible with validation layers on.
  constexpr size_t kClearedBufferCount = 5;
  const std::array<const nvvk::Buffer*, kClearedBufferCount> buffers{
      &m_Resources.GetPathReservoirBuffer(),
      &m_Resources.GetSurfaceBuffer(0),
      &m_Resources.GetSurfaceBuffer(1),
      // A stale duplication score would throttle the cap on history that no longer
      // exists, so it is cleared with everything else it describes.
      &m_Resources.GetDuplicationBuffer(),
      // Likewise a motion vector describing a camera pose that no longer applies.
      &m_Resources.GetMotionVectorBuffer(),
  };

  // A previous frame may still be executing the passes that write these buffers,
  // and every fill below is a write. Without this the fill races that shader write
  // as a write-after-write hazard: the barrier AFTER the fills orders the fills
  // against later reads, but nothing can retroactively order them against earlier
  // work. Both the source stages and TRANSFER_WRITE on the destination side are
  // spelled out because the inferred masks would produce TRANSFER_READ, which does
  // not cover a fill.
  const VkMemoryBarrier2 clearWriteBarrier{
      .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
      .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
  };
  const VkDependencyInfo clearWriteDependency{
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &clearWriteBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &clearWriteDependency);

  std::array<VkBufferMemoryBarrier2, kClearedBufferCount> barriers{};
  for(size_t i = 0; i < buffers.size(); ++i)
  {
    vkCmdFillBuffer(cmd, buffers[i]->buffer, 0, buffers[i]->bufferSize, 0);
    barriers[i] = VkBufferMemoryBarrier2{
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

  // Transfer writes must be visible before ray tracing/compute reads them.
  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = uint32_t(barriers.size()),
      .pBufferMemoryBarriers    = barriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

// Section 6.2.2's compacted pre-pass. Plain on/off: an accumulation-based heuristic
// was tried and removed, because the divergence it guards against also occurs with a
// moving camera and accumulation off (see ReSTIRPTSettings.h).
bool ReSTIRPTRenderer::UseSortedPrepass() const
{
  return m_Settings.common.sortPrepass != 0u;
}

void ReSTIRPTRenderer::RecordPasses(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  // Order this frame's first writes against the previous frame's last reads.
  //
  // Every buffer this renderer owns is a single allocation shared by all frames in
  // flight - reservoirs, surfaces, paired shifts, shading weights, motion vectors.
  // Only the descriptor SETS rotate. Submission order does not imply an execution
  // or memory dependency, command buffer boundaries add none, and nvapp's fence
  // waits for the previous use of this frame's ring slot rather than for the
  // immediately preceding frame - which, with two frames in flight, is not the
  // frame whose reads must complete first.
  //
  // Unconditional on purpose. The only other barrier that could serve is the one
  // after the duplication map, and that is conditional on decorrelation and
  // temporal reuse both being enabled, so it cannot establish this for every mode.
  // It matters most with two reservoir arrays, where initial sampling overwrites
  // the array the previous frame's spatial pass read neighbours from, leaving no
  // slack at all.
  // TRANSFER and DRAW_INDIRECT are in scope as well as the shader stages, because
  // two of this renderer's cross-frame accesses are neither: the prepass counter is
  // cleared with vkCmdFillBuffer and then read as indirect trace arguments, and the
  // diagnostic readback copies the debug buffer. Leaving those out let frame N+1's
  // clear race frame N's indirect read - which silently shrinks a dispatch, and so
  // looks like a speedup rather than a fault.
  constexpr VkPipelineStageFlags2 kFrameStages = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
                                                 | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                                 | VK_PIPELINE_STAGE_2_TRANSFER_BIT
                                                 | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  CmdReSTIRPTMemoryBarrier(input.cmd, kFrameStages, kFrameStages);

  // Section 6.1. Presampling has to complete before any pixel reads a tile, and it
  // depends on nothing this frame produces, so it goes first.
  if(m_Settings.nee.enableLightTiles != 0u)
  {
    {
      RunLightTilePass(input, pushConstant);
    }
    CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  }

  {
    RunInitialSamplingPass(input, pushConstant);
  }

  if(IsReSTIRPTTemporalResamplingEnabled(m_Settings.common.resamplingMode))
  {
    // Initial sampling writes the candidate reservoirs that temporal reuse reads.
    CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                           VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    RunTemporalPass(input, pushConstant);
  }

  if(IsReSTIRPTSpatialResamplingEnabled(m_Settings.common.resamplingMode))
  {
    // Spatial reads a neighbourhood of the array the previous pass wrote, so the
    // whole pass must be visible - not just this pixel's own element.
    //
    // COMPUTE is in the destination scope because the sorted pre-pass begins with a
    // COMPUTE dispatch, not a ray-tracing one: its classify pass reads the surfaces
    // and reservoirs the temporal pass writes. Ray tracing alone was correct until a
    // compute pass was put in front of this barrier, and then silently was not.
    //
    // This is a real missing dependency, but be aware it is NOT the cause of the
    // sorted pre-pass divergence documented in ReSTIRPTSettings.h - that survives
    // full ALL_COMMANDS serialization of the whole pass, so it is not a
    // synchronization fault at all. Adding this changed nothing observable.
    CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                           VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    // Section 3. Each pixel shifts its own path into its partner's domain and
    // publishes the result; resampling then reads both its own record and its
    // partner's instead of tracing either shift. The barrier between is exactly
    // what makes the sharing possible - and is the overhead the paper cites for
    // why the saving is not a full 2x.
    if(m_Settings.spatialResampling.enablePairedSpatialReuse != 0u)
    {
      // Section 6.2.2. The sorted path times its own sub-passes internally.
      if(UseSortedPrepass())
      {
        RunSortedSpatialPrepass(input, pushConstant);
      }
      else
      {
        RunSpatialPrepass(input, pushConstant);
      }
      CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    }

    {
      RunSpatialPass(input, pushConstant);
    }
  }

  // The reuse passes write the reservoirs that final shading reads.
  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  {
    RunFinalShadingPass(input, pushConstant);
  }
  // Leave the image writes visible to post processing.
  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  // Section 5's map describes the reservoirs this frame ended with, so it runs
  // last - after every pass that can still change them. It reads the same array
  // final shading just read, and the barrier above already orders it. Only the
  // next frame's temporal pass consumes the result, which is why nothing here
  // waits on it; the frame boundary provides that ordering.
  //
  // Skipped when temporal reuse is off, since nothing would ever read the map.
  if(m_Settings.decorrelation.enable != 0u && IsReSTIRPTTemporalResamplingEnabled(m_Settings.common.resamplingMode))
  {
    {
      RunDuplicationMapPass(input, pushConstant);
    }
    CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  }

}
void ReSTIRPTRenderer::RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR PT Initial Sampling");
  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  TraceReSTIRRayTracingPass(input.cmd, m_InitialSamplingPass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                              kReSTIRPTPushConstantStages, pushConstant, input.gBuffers->getSize());
}

void ReSTIRPTRenderer::RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR PT Temporal Resampling");
  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  TraceReSTIRRayTracingPass(input.cmd, m_TemporalPass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                              kReSTIRPTPushConstantStages, pushConstant, input.gBuffers->getSize());
}

void ReSTIRPTRenderer::RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR PT Spatial Resampling");
  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  TraceReSTIRRayTracingPass(input.cmd, m_SpatialPass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                              kReSTIRPTPushConstantStages, pushConstant, input.gBuffers->getSize());
}

void ReSTIRPTRenderer::RunSortedSpatialPrepass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR PT Spatial Prepass (sorted)");
  const uint32_t       frameSetIndex = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  VkDescriptorSet      descriptorSet = *m_DescPack.getSetPtr(frameSetIndex);
  const nvvk::Buffer&  counters      = m_Resources.GetPrepassCounterBuffer();
  const VkExtent2D     viewportSize  = input.gBuffers->getSize();

  // Counts and cursors accumulate with atomics, so they must start at zero every
  // frame. The offsets and indirect dimensions are fully rewritten below, so
  // clearing the whole buffer costs nothing extra.
  //
  // The barrier before it is a write-after-read across the FRAME boundary, and it
  // is not covered by the general one at the top of RecordPasses: that one spans
  // ray tracing and compute, while this is a TRANSFER write. This buffer is a
  // single allocation shared by every frame in flight, so without this the fill
  // can land while the previous frame is still reading the counts - which
  // corrupts the launch size, and makes the traced pass read work list entries
  // that were never scattered.
  CmdReSTIRPTMemoryBarrier(input.cmd,
                         VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                             | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT);
  vkCmdFillBuffer(input.cmd, counters.buffer, 0, counters.bufferSize, 0);
  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  // 1. Append the pairs that need a shift to the work list, and write the
  //    paired-shift record for every REJECTED pair - the traced pass never visits
  //    those, and a record left untouched would be read next pass as a live shift
  //    from an earlier frame.
  {
    DispatchReSTIRComputePass(input.cmd, m_PrepassClassifyPipeline, m_PipelineLayout, descriptorSet,
                              kReSTIRPTPushConstantStages, pushConstant, viewportSize, kPrepassSortGroupSize);
  }
  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  // 2. Publish the launch size the first pass counted.
  {
    vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PrepassOffsetsPipeline);
    vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdPushConstants(input.cmd, m_PipelineLayout, kReSTIRPTPushConstantStages, 0, sizeof(pushConstant), &pushConstant);
    vkCmdDispatch(input.cmd, 1, 1, 1);
  }
  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);


  // The work list is read by the ray tracing stage, and the same buffer's tail is
  // read by the indirect-draw stage as the launch dimensions. Both dependencies
  // are on this one barrier.
  CmdReSTIRPTMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);

  // 4. Trace exactly the surviving pairs, in bucket order. The count lives only on
  //    the GPU, which is what makes this dispatch indirect.
  {
    vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_SpatialPrepass.pipeline);
    vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdPushConstants(input.cmd, m_PipelineLayout, kReSTIRPTPushConstantStages, 0, sizeof(pushConstant), &pushConstant);
    // Plain vkCmdTraceRaysIndirectKHR: only the dimensions come from the buffer,
    // and the shader binding table is still supplied here. That is core to
    // VK_KHR_ray_tracing_pipeline, so it needs no extra extension - unlike the
    // Indirect2 variant, which also indirects the binding table.
    vkCmdTraceRaysIndirectKHR(input.cmd, &m_SpatialPrepass.sbtRegions.raygen, &m_SpatialPrepass.sbtRegions.miss,
                              &m_SpatialPrepass.sbtRegions.hit, &m_SpatialPrepass.sbtRegions.callable,
                              counters.address + VkDeviceSize(RESTIR_PT_PREPASS_INDIRECT_OFFSET) * sizeof(uint32_t));
  }
}

void ReSTIRPTRenderer::RunSpatialPrepass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR PT Spatial Prepass");
  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  TraceReSTIRRayTracingPass(input.cmd, m_SpatialPrepass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                               kReSTIRPTPushConstantStages, pushConstant, input.gBuffers->getSize());
}

void ReSTIRPTRenderer::RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR PT Final Shading");
  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  DispatchReSTIRComputePass(input.cmd, m_FinalShadingPipeline, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                              kReSTIRPTPushConstantStages, pushConstant, input.gBuffers->getSize(), kComputeGroupSize);
}

void ReSTIRPTRenderer::RunLightTilePass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR PT Light Tiles");
  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  // The dispatch is shaped by the tile table, not the viewport: one thread per
  // presampled light, x along a tile and y across tiles.
  const VkExtent2D tileExtent{.width = uint32_t(RESTIR_PT_LIGHT_TILE_SIZE), .height = uint32_t(RESTIR_PT_LIGHT_TILE_COUNT)};
  DispatchReSTIRComputePass(input.cmd, m_LightTilePipeline, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                            kReSTIRPTPushConstantStages, pushConstant, tileExtent, kLightTileGroupSize);
}

void ReSTIRPTRenderer::RunDuplicationMapPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR PT Duplication Map");
  const uint32_t frameSetIndex = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  // The shader tiles its shared-memory window to an 8x8 group, so the dispatch
  // must use that group size and not the shared compute default.
  DispatchReSTIRComputePass(input.cmd, m_DuplicationMapPipeline, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                            kReSTIRPTPushConstantStages, pushConstant, input.gBuffers->getSize(), kDuplicationMapGroupSize);
}

void ReSTIRPTRenderer::FinishFrame(const FrameState& frameState)
{
  m_LastAccumulationSignature = frameState.accumulationSignature;
  m_HasAccumulationSignature  = true;
  m_LastDenoiserSignature     = frameState.denoiserSignature;
  m_HasDenoiserSignature      = true;
  m_HistoryInvalidated        = false;
  m_FrameContext.AdvanceFrame();
  m_AccumulatedFrames = IsAccumulationResolveMode(m_Settings.common.resolveMode) ? (m_AccumulatedFrames + 1u) : 0u;
}

namespace {
}  // namespace
void ReSTIRPTRenderer::QueryRayTracingProperties()
{
  VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m_RtProperties};
  vkGetPhysicalDeviceProperties2(m_Allocator->getPhysicalDevice(), &props);

  // Vulkan recursion depth counts the primary ray, so path bounces get one less.
  // The reconnection-length field caps it again: a path longer than that field can
  // represent could not record where it reconnected.
  const uint32_t maxBounceLimit = (m_RtProperties.maxRayRecursionDepth > 0) ? (m_RtProperties.maxRayRecursionDepth - 1u) : 0u;
  m_PipelineBounceLimit = std::min(std::min(kRequestedMaxBounces, maxBounceLimit), uint32_t(RESTIR_PT_MAX_BOUNCES));
  m_Settings.initialSampling.maxBounces = std::min(m_Settings.initialSampling.maxBounces, m_PipelineBounceLimit);
}

void ReSTIRPTRenderer::CreateDescriptorSetLayout()
{
  // One descriptor layout is shared by all ReSTIR PT passes so the pass sequence
  // can bind the same set for both the ray tracing and compute stages.
  const VkShaderStageFlags allStages = kReSTIRPTPushConstantStages;

  nvvk::DescriptorBindings bindings;
  // Textures use bindless-style indexing from material records.
  bindings.addBinding({.binding         = shaderio::ReSTIRPTBindingPoints::eReSTIRPTTextures,
                       .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       .descriptorCount = m_MaxTextureDescriptors,
                       .stageFlags      = allStages},
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                          | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTOutputImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTAccumulationImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPathReservoirBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  // Surface buffers ping-pong for current/previous-frame temporal reuse.
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTCurrentSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPreviousSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTParamsBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTDuplicationBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPairingBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPairedShiftBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTShadingWeightBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTMotionVectorBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTLightTileBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPrepassWorkBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTPrepassCounterBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTDenoiserGuideBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  // NRD inputs. Bound unconditionally so one descriptor layout serves every resolve
  // mode; final shading writes them only when the denoiser-signal flag is set.
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTMotionVectorsImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTNormalRoughnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTBaseColorMetalnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTViewZImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTDiffuseRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                      1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTSpecularRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                      1, allStages);
  bindings.addBinding(shaderio::ReSTIRPTBindingPoints::eReSTIRPTSpecularDemodulationFactorImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                      1, allStages);

  m_DescPack.init(bindings, m_Allocator->getDevice(), m_App->getFrameCycleSize(),
                  VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                  VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
}

void ReSTIRPTRenderer::CreatePipelineLayout()
{
  // Pipeline layout is the ABI between C++ descriptor sets/push constants and Slang bindings.
  const VkPushConstantRange pushConstantRange{
      .stageFlags = kReSTIRPTPushConstantStages,
      .offset     = 0,
      .size       = sizeof(shaderio::ReSTIRPTPushConstant),
  };

  const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = m_DescPack.getLayoutPtr(),
      .pushConstantRangeCount = 1,
      .pPushConstantRanges    = &pushConstantRange,
  };

  NVVK_CHECK(vkCreatePipelineLayout(m_Allocator->getDevice(), &pipelineLayoutInfo, nullptr, &m_PipelineLayout));
  NVVK_DBG_NAME(m_PipelineLayout);
}

void ReSTIRPTRenderer::CreateParameterBuffers()
{
  const uint32_t frameSetCount = std::max(1u, m_App->getFrameCycleSize());
  m_ParameterBuffers.resize(frameSetCount);

  for(nvvk::Buffer& parameterBuffer : m_ParameterBuffers)
  {
    // Mapped uniform buffers are updated once per frame set before recording passes.
    NVVK_CHECK(m_Allocator->createBuffer(parameterBuffer, sizeof(shaderio::ReSTIRPTParameters),
                                         VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                         VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT));
    NVVK_DBG_NAME(parameterBuffer.buffer);
  }
}

void ReSTIRPTRenderer::CreateInitialSamplingPipeline()
{
  // Initial sampling recurses once per bounce, so the pipeline's recursion budget
  // must cover the primary ray plus every bounce the UI can request.
  CreateReSTIRRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetInitialSamplingShaderCode(),
                               std::max(1u, m_PipelineBounceLimit + 1u), "ReSTIR PT Initial Sampling Pipeline", m_InitialSamplingPass);
}

void ReSTIRPTRenderer::CreateTemporalPipeline()
{
  // Replay traces one ray per regenerated bounce from the ray generation loop, so
  // the pipeline only ever needs depth-1 recursion regardless of path length.
  CreateReSTIRRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetTemporalShaderCode(), 2u,
                               "ReSTIR PT Temporal Resampling Pipeline", m_TemporalPass);
}

void ReSTIRPTRenderer::CreateSpatialPipeline()
{
  CreateReSTIRRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetSpatialShaderCode(), 2u,
                               "ReSTIR PT Spatial Resampling Pipeline", m_SpatialPass);
  CreateReSTIRRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetSpatialPrepassShaderCode(), 2u,
                               "ReSTIR PT Spatial Prepass Pipeline", m_SpatialPrepass);
}

void ReSTIRPTRenderer::CreateFinalShadingPipeline()
{
  m_FinalShadingPipeline =
      CreateReSTIRComputePipeline(m_Allocator, m_PipelineLayout, GetFinalShadingShaderCode(), "ReSTIR PT Final Shading Pipeline");
  m_DuplicationMapPipeline = CreateReSTIRComputePipeline(m_Allocator, m_PipelineLayout, GetDuplicationMapShaderCode(),
                                                         "ReSTIR PT Duplication Map Pipeline");
  m_LightTilePipeline      = CreateReSTIRComputePipeline(m_Allocator, m_PipelineLayout, GetLightTilesShaderCode(),
                                                         "ReSTIR PT Light Tiles Pipeline");
  m_PrepassClassifyPipeline = CreateReSTIRComputePipeline(m_Allocator, m_PipelineLayout, GetPrepassClassifyShaderCode(),
                                                          "ReSTIR PT Prepass Classify Pipeline");
  m_PrepassOffsetsPipeline  = CreateReSTIRComputePipeline(m_Allocator, m_PipelineLayout, GetPrepassOffsetsShaderCode(),
                                                          "ReSTIR PT Prepass Offsets Pipeline");
}

void ReSTIRPTRenderer::UpdateFrameDescriptors(const RenderInput& input)
{
  const uint32_t frameSetIndex        = GetReSTIRPTFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  const uint32_t currentHistoryIndex  = m_FrameContext.GetCurrentHistoryIndex();
  const uint32_t previousHistoryIndex = m_FrameContext.GetPreviousHistoryIndex();

  VkDescriptorImageInfo outputImageInfo = input.gBuffers->getDescriptorImageInfo(input.renderedImageIndex);
  outputImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo accumulationImageInfo = m_Resources.GetAccumulationImage().descriptor;
  accumulationImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  // NRD guide images are always bound; final shading only writes them when the flag
  // is set. GENERAL is the layout the transitions above leave them in.
  VkDescriptorImageInfo motionVectorsImageInfo      = m_DenoiserResources.GetMotionVectorsImage().descriptor;
  motionVectorsImageInfo.imageLayout                = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo normalRoughnessImageInfo    = m_DenoiserResources.GetNormalRoughnessImage().descriptor;
  normalRoughnessImageInfo.imageLayout              = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo baseColorMetalnessImageInfo = m_DenoiserResources.GetBaseColorMetalnessImage().descriptor;
  baseColorMetalnessImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo viewZImageInfo              = m_DenoiserResources.GetViewZImage().descriptor;
  viewZImageInfo.imageLayout                        = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo diffuseRadianceHitDistanceImageInfo = m_DenoiserResources.GetDiffuseRadianceHitDistanceImage().descriptor;
  diffuseRadianceHitDistanceImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo specularRadianceHitDistanceImageInfo = m_DenoiserResources.GetSpecularRadianceHitDistanceImage().descriptor;
  specularRadianceHitDistanceImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo specularDemodulationFactorImageInfo = m_DenoiserResources.GetSpecularDemodulationFactorImage().descriptor;
  specularDemodulationFactorImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  const std::array<VkDescriptorBufferInfo, 13> bufferInfos{
      // One buffer holds every rotating reservoir array.
      VkDescriptorBufferInfo{m_Resources.GetPathReservoirBuffer().buffer, 0, VK_WHOLE_SIZE},
      // Surface buffers are bound as current/previous according to frame parity.
      VkDescriptorBufferInfo{m_Resources.GetSurfaceBuffer(currentHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetSurfaceBuffer(previousHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_ParameterBuffers[frameSetIndex].buffer, 0, sizeof(shaderio::ReSTIRPTParameters)},
      VkDescriptorBufferInfo{m_Resources.GetDuplicationBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetPairingBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetPairedShiftBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetShadingWeightBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetMotionVectorBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetLightTileBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetPrepassWorkBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetPrepassCounterBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetDenoiserGuideBuffer().buffer, 0, VK_WHOLE_SIZE},
  };

  VkAccelerationStructureKHR accel = input.topLevelAS->accel;
  // TLAS descriptors attach through pNext rather than pBufferInfo/pImageInfo.
  const VkWriteDescriptorSetAccelerationStructureKHR accelerationInfo{
      .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
      .accelerationStructureCount = 1,
      .pAccelerationStructures    = &accel,
  };

  std::array<VkWriteDescriptorSet, 24> writes{};
  uint32_t                            writeCount = 0;

  writes[writeCount]       = m_DescPack.makeWrite(shaderio::ReSTIRPTBindingPoints::eReSTIRPTTlas, frameSetIndex);
  writes[writeCount].pNext = &accelerationInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.makeWrite(shaderio::ReSTIRPTBindingPoints::eReSTIRPTOutputImage, frameSetIndex);
  writes[writeCount].pImageInfo = &outputImageInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.makeWrite(shaderio::ReSTIRPTBindingPoints::eReSTIRPTAccumulationImage, frameSetIndex);
  writes[writeCount].pImageInfo = &accumulationImageInfo;
  ++writeCount;

  const std::array<std::pair<uint32_t, const VkDescriptorImageInfo*>, 7> denoiserImageBindings{{
      {shaderio::ReSTIRPTBindingPoints::eReSTIRPTMotionVectorsImage, &motionVectorsImageInfo},
      {shaderio::ReSTIRPTBindingPoints::eReSTIRPTNormalRoughnessImage, &normalRoughnessImageInfo},
      {shaderio::ReSTIRPTBindingPoints::eReSTIRPTBaseColorMetalnessImage, &baseColorMetalnessImageInfo},
      {shaderio::ReSTIRPTBindingPoints::eReSTIRPTViewZImage, &viewZImageInfo},
      {shaderio::ReSTIRPTBindingPoints::eReSTIRPTDiffuseRadianceHitDistanceImage, &diffuseRadianceHitDistanceImageInfo},
      {shaderio::ReSTIRPTBindingPoints::eReSTIRPTSpecularRadianceHitDistanceImage, &specularRadianceHitDistanceImageInfo},
      {shaderio::ReSTIRPTBindingPoints::eReSTIRPTSpecularDemodulationFactorImage, &specularDemodulationFactorImageInfo},
  }};

  for(const auto& [binding, imageInfo] : denoiserImageBindings)
  {
    writes[writeCount]            = m_DescPack.makeWrite(binding, frameSetIndex);
    writes[writeCount].pImageInfo = imageInfo;
    ++writeCount;
  }

  const std::array<uint32_t, 13> bufferBindings{
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
    writes[writeCount]             = m_DescPack.makeWrite(bufferBindings[i], frameSetIndex);
    writes[writeCount].pBufferInfo = &bufferInfos[i];
    ++writeCount;
  }

  vkUpdateDescriptorSets(m_Allocator->getDevice(), writeCount, writes.data(), 0, nullptr);
}

void ReSTIRPTRenderer::UpdateParameterBuffer(uint32_t frameSetIndex, const shaderio::ReSTIRPTParameters& parameters)
{
  if(frameSetIndex >= m_ParameterBuffers.size())
  {
    return;
  }

  // Host writes to the mapped uniform buffer are flushed before recording uses it.
  nvvk::Buffer& parameterBuffer = m_ParameterBuffers[frameSetIndex];
  std::memcpy(parameterBuffer.mapping, &parameters, sizeof(parameters));
  NVVK_CHECK(m_Allocator->flushBuffer(parameterBuffer, 0, sizeof(parameters)));
}

}  // namespace nvsamples
