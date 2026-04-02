#include "ReSTIRPTRenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include <nvapp/application.hpp>
#include <nvvk/barriers.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

#include "Common/Utils.hpp"
#include "ReSTIR/Common/ReSTIRUtils.h"

#include "_autogen/ReSTIRPTFinalShading.slang.h"
#include "_autogen/ReSTIRPTGenerateInitialSamples.slang.h"
#include "_autogen/ReSTIRPTSpatialResampling.slang.h"
#include "_autogen/ReSTIRPTTemporalResampling.slang.h"

namespace nvsamples
{

namespace
{

constexpr uint32_t kRequestedMaxBounces = 8;
constexpr uint32_t kComputeGroupSize    = 8;
// One push-constant block is shared by the RT and compute stages so each pass
// can switch behavior without rebinding different layouts.
constexpr VkShaderStageFlags kReSTIRPTPushConstantStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR
                                                           | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR
                                                           | VK_SHADER_STAGE_COMPUTE_BIT;

void TransitionStorageImageForWrite(VkCommandBuffer cmd, nvvk::Image& image, VkPipelineStageFlags2 dstStageMask)
{
  if(image.image == VK_NULL_HANDLE)
  {
    return;
  }

  if(image.descriptor.imageLayout == VK_IMAGE_LAYOUT_GENERAL)
  {
    const VkImageMemoryBarrier2 imageBarrier{
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask  = dstStageMask,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .image         = image.image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
    };
    const VkDependencyInfo dependencyInfo{
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &imageBarrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
    return;
  }

  const VkImageMemoryBarrier2 imageBarrier{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask  = dstStageMask,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout     = image.descriptor.imageLayout,
      .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .image         = image.image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
  };
  const VkDependencyInfo dependencyInfo{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &imageBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

VkShaderModuleCreateInfo GetInitialSamplingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTGenerateInitialSamples_slang));
}

VkShaderModuleCreateInfo GetTemporalShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTTemporalResampling_slang));
}

VkShaderModuleCreateInfo GetSpatialShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTSpatialResampling_slang));
}

VkShaderModuleCreateInfo GetFinalShadingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRPTFinalShading_slang));
}

}  // namespace

ReSTIRPTRenderer::ReSTIRPTRenderer(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
    , m_Resources(ReSTIRResources::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
    , m_DenoiserResources(PathTraceDenoiserResources::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
    , m_NrdDenoiser(PathTraceNrdDenoiser::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
{
}

void ReSTIRPTRenderer::Initialize()
{
  if(m_App == nullptr || m_Allocator == nullptr || m_MaxTextureDescriptors == 0)
  {
    return;
  }

  // The renderer builds all passes up front because the descriptor/pipeline
  // contract is stable across frames.
  QueryRayTracingProperties();
  CreateDescriptorSetLayout();
  CreatePipelineLayout();
  CreateInitialSamplingPipeline();
  CreateFinalShadingPipeline();
  CreateComputePipelines();
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
  m_NrdDenoiser.Destroy();
  m_DenoiserResources.Destroy();
  m_Resources.Destroy();

  DestroyReSTIRRayTracingPass(m_Allocator, m_InitialSamplingPass);
  DestroyReSTIRRayTracingPass(m_Allocator, m_FinalShadingPass);

  for(VkPipeline& computePipeline : m_ComputePipelines)
  {
    vkDestroyPipeline(device, computePipeline, nullptr);
    computePipeline = VK_NULL_HANDLE;
  }

  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
  m_PipelineLayout = VK_NULL_HANDLE;

  m_DescPack.deinit();
  m_AccumulatedFrames = 0;
  m_HistoryInvalidated = true;
  m_HasHistorySignature = false;
  m_HasDenoiserSignature = false;
}

bool ReSTIRPTRenderer::IsReady() const
{
  return m_PipelineLayout != VK_NULL_HANDLE && IsReSTIRRayTracingPassReady(m_InitialSamplingPass) && IsReSTIRRayTracingPassReady(m_FinalShadingPass)
         && m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)] != VK_NULL_HANDLE
         && m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)] != VK_NULL_HANDLE;
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

void ReSTIRPTRenderer::InvalidateHistory()
{
  // This resets both final accumulation and the temporal reservoir chain.
  m_AccumulatedFrames = 0;
  m_HistoryInvalidated = true;
  m_HasHistorySignature = false;
  m_HasDenoiserSignature = false;
  m_Context.InvalidateHistory();
  m_NrdDenoiser.InvalidateHistory();
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
  if(!IsReady() || input.cmd == VK_NULL_HANDLE || input.sceneResource == nullptr || input.sceneInfo == nullptr || input.topLevelAS == nullptr
     || input.topLevelAS->accel == VK_NULL_HANDLE || input.gBuffers == nullptr)
  {
    return;
  }

  const VkExtent2D viewportSize = input.gBuffers->getSize();
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  m_Context.EnsureViewport(viewportSize);
  m_Resources.EnsureForViewport(viewportSize);
  m_DenoiserResources.EnsureForViewport(viewportSize);

  // Camera-driven invalidation is only needed when final accumulation is
  // active. Temporal ReSTIR reuse is allowed to follow normal camera motion.
  const HistorySignature currentSignature = MakeReSTIRHistorySignature(*input.sceneInfo, input.topLevelAS->address, viewportSize);
  const bool signatureChanged = !m_HasHistorySignature
                                || std::memcmp(&currentSignature, &m_LastHistorySignature, sizeof(HistorySignature)) != 0;
  const bool denoiseEnabled = IsDenoiseResolveMode(m_Settings.common.resolveMode);
  const bool restirHistoryInvalidated =
      m_HistoryInvalidated || (IsAccumulationResolveMode(m_Settings.common.resolveMode) && signatureChanged);
  const DenoiserSignature currentDenoiserSignature =
      MakeReSTIRDenoiserHistorySignature(*input.sceneInfo, input.topLevelAS->address, viewportSize);
  const bool denoiserSignatureChanged = !m_HasDenoiserSignature
                                        || std::memcmp(&currentDenoiserSignature, &m_LastDenoiserSignature, sizeof(DenoiserSignature)) != 0;
  const bool denoiserHistoryInvalidated = m_HistoryInvalidated || (denoiseEnabled && denoiserSignatureChanged);
  if(restirHistoryInvalidated)
  {
    m_AccumulatedFrames = 0;
    m_Context.InvalidateHistory();
  }

  const bool restirDebugActive = m_Settings.common.debugView != shaderio::eReSTIRDebugViewDisabled;
  if(denoiseEnabled)
  {
    m_NrdDenoiser.PrepareFrame(PathTraceNrdDenoiser::FrameInput{
                                   .sceneInfo          = input.sceneInfo,
                                   .viewportSize       = viewportSize,
                                   .historyInvalidated = denoiserHistoryInvalidated,
                                   .enableMaterialDemodulation = true,
                                   .settings           = &m_Settings.common.denoiserSettings,
                               },
                               m_DenoiserResources);
  }

  UpdateFrameDescriptors(input);
  // The passes write storage images and storage buffers, so the output images
  // must be in GENERAL before the first dispatch.
  TransitionReSTIRStorageImages(input.cmd, const_cast<nvvk::Image&>(m_Resources.GetAccumulationImage()),
                                input.gBuffers->getColorImage(input.renderedImageIndex),
                                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetMotionVectorsImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetNormalRoughnessImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetBaseColorMetalnessImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetViewZImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetDiffuseRadianceHitDistanceImage(),
                                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetSpecularRadianceHitDistanceImage(),
                                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);

  const bool enableTemporal = IsReSTIRTemporalResamplingEnabled(m_Settings.common.resamplingMode);
  const bool enableSpatial  = IsReSTIRSpatialResamplingEnabled(m_Settings.common.resamplingMode);

  shaderio::ReSTIRInitialSamplingParameters initialSampling = m_Settings.common.initialSampling;
  initialSampling.maxBounceDepth = std::min(initialSampling.maxBounceDepth, m_PipelineBounceLimit);
  shaderio::ReSTIRTemporalResamplingParameters temporalResampling = m_Settings.common.temporalResampling;
  temporalResampling.uniformRandomNumber = restir::JenkinsHash(m_Context.GetFrameIndex());

  const shaderio::ReSTIRPTPushConstant pushConstant{
      .sceneInfoAddress            = (shaderio::GltfSceneInfo*)input.sceneResource->bSceneInfo.address,
      .rngFrameNumber              = m_Context.GetFrameIndex(),
      .accumulatedFrames           = IsAccumulationResolveMode(m_Settings.common.resolveMode) ? m_AccumulatedFrames : 0u,
      .flags                       = BuildFrameFlags(enableTemporal, enableSpatial),
      .initialSampling             = initialSampling,
      .temporalResampling          = temporalResampling,
      .spatialResampling           = m_Settings.common.spatialResampling,
      .boilingFilter               = m_Settings.common.boilingFilter,
      .reconnection                = m_Settings.reconnection,
      .enableVisibilityValidation  = m_Settings.common.enableVisibilityValidation ? 1u : 0u,
      .neighborOffsetCount         = m_Resources.GetNeighborOffsetCount(),
      .debugView                   = static_cast<uint32_t>(m_Settings.common.debugView),
      .pathTraceInvocationType     = shaderio::eReSTIRPTPathTraceInvocationTypeNone,
  };

  RunInitialSamplingPass(input, pushConstant);

  if(enableTemporal)
  {
    // Initial sampling produces the per-pixel candidates consumed by temporal
    // reuse, so the compute pass waits on RT writes here.
    nvvk::cmdMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    RunTemporalPass(input, pushConstant);
  }

  if(enableSpatial)
  {
    // Spatial reuse reads whichever buffer the previous stage wrote: either the
    // initial candidates or the temporal output scratch buffer.
    const VkPipelineStageFlags2 srcStage = enableTemporal ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    nvvk::cmdMemoryBarrier(input.cmd, srcStage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    RunSpatialPass(input, pushConstant);
  }

  VkPipelineStageFlags2 finalInputStage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
  if(enableTemporal || enableSpatial)
  {
    finalInputStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  }

  // Final replay reads the last reservoir buffer state and writes the resolved
  // radiance into the accumulation/output images.
  nvvk::cmdMemoryBarrier(input.cmd, finalInputStage, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  RunFinalShadingPass(input, pushConstant);
  nvvk::cmdMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  if(denoiseEnabled && !restirDebugActive && m_NrdDenoiser.IsReady())
  {
    m_NrdDenoiser.Denoise(input.cmd, m_DenoiserResources, m_Resources.GetAccumulationImage().descriptor.imageView,
                          input.gBuffers->getColorImageView(input.renderedImageIndex), m_Settings.common.denoiserDebugView, viewportSize);
  }
  else if(denoiseEnabled && restirDebugActive)
  {
    m_NrdDenoiser.InvalidateHistory();
  }

  m_LastHistorySignature = currentSignature;
  m_LastDenoiserSignature = currentDenoiserSignature;
  m_HasHistorySignature  = true;
  m_HasDenoiserSignature = true;
  m_HistoryInvalidated   = false;
  m_Context.AdvanceFrame();
  m_AccumulatedFrames = IsAccumulationResolveMode(m_Settings.common.resolveMode) ? (m_AccumulatedFrames + 1u) : 0u;
}

uint32_t ReSTIRPTRenderer::BuildFrameFlags(bool enableTemporal, bool enableSpatial) const
{
  uint32_t flags = 0;
  if(IsAccumulationResolveMode(m_Settings.common.resolveMode))
  {
    flags |= shaderio::eReSTIRFlagAccumulate;
  }
  if(enableTemporal)
  {
    flags |= shaderio::eReSTIRFlagEnableTemporal;
  }
  if(enableSpatial)
  {
    flags |= shaderio::eReSTIRFlagEnableSpatial;
  }
  if(m_Context.HasHistory())
  {
    flags |= shaderio::eReSTIRFlagHasHistory;
  }
  if(!enableTemporal && !enableSpatial)
  {
    flags |= shaderio::eReSTIRFlagInitialWriteToHistory;
  }
  if(enableTemporal && enableSpatial)
  {
    // Temporal writes scratch so spatial can read a stable snapshot without
    // clobbering the final history buffer mid-frame.
    flags |= shaderio::eReSTIRFlagTemporalWriteToScratch;
    flags |= shaderio::eReSTIRFlagSpatialReadFromScratch;
  }

  return flags;
}

void ReSTIRPTRenderer::QueryRayTracingProperties()
{
  VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m_RtProperties};
  vkGetPhysicalDeviceProperties2(m_Allocator->getPhysicalDevice(), &props);

  // The actual pipeline recursion depth cannot exceed the physical device's RT
  // limit, even if the UI requests more bounces.
  m_MaxBounceLimit      = (m_RtProperties.maxRayRecursionDepth > 0) ? (m_RtProperties.maxRayRecursionDepth - 1u) : 0u;
  m_PipelineBounceLimit = std::min(kRequestedMaxBounces, m_MaxBounceLimit);
  m_Settings.common.initialSampling.maxBounceDepth = std::min(m_Settings.common.initialSampling.maxBounceDepth, m_PipelineBounceLimit);
}

void ReSTIRPTRenderer::CreateDescriptorSetLayout()
{
  const VkShaderStageFlags allStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR
                                       | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR
                                       | VK_SHADER_STAGE_COMPUTE_BIT;

  // All passes share one descriptor set layout so the pass transition cost is
  // just pipeline binding plus push-constant updates.
  nvvk::DescriptorBindings bindings;
  bindings.addBinding({.binding = shaderio::ReSTIRBindingPoints::eReSTIRTextures,
                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       .descriptorCount = m_MaxTextureDescriptors,
                       .stageFlags = allStages},
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                          | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIROutputImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRAccumulationImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRInitialReservoirBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRScratchReservoirBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRCurrentHistoryReservoirBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRPreviousHistoryReservoirBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRCurrentSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRPreviousSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRNeighborOffsetBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRDebugBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRMotionVectorsImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRNormalRoughnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRBaseColorMetalnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRViewZImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRDiffuseRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRBindingPoints::eReSTIRSpecularRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);

  m_DescPack.init(bindings, m_Allocator->getDevice(), m_App->getFrameCycleSize(), VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                  VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
}

void ReSTIRPTRenderer::CreatePipelineLayout()
{
  // The pipeline layout is the CPU-side declaration of the descriptor/push
  // constant contract consumed in Globals.h.slang.
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

void ReSTIRPTRenderer::CreateInitialSamplingPipeline()
{
  CreateReSTIRRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetInitialSamplingShaderCode(),
                             std::max(1u, m_PipelineBounceLimit + 1u), m_InitialSamplingPass);
}

void ReSTIRPTRenderer::CreateFinalShadingPipeline()
{
  CreateReSTIRRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetFinalShadingShaderCode(),
                             std::max(1u, m_PipelineBounceLimit + 1u), m_FinalShadingPass);
}

void ReSTIRPTRenderer::CreateComputePipelines()
{
  // Temporal and spatial passes share the same descriptor layout but use
  // different entry points and dispatch logic.
  m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)] =
      CreateReSTIRComputePipeline(m_Allocator, m_PipelineLayout, GetTemporalShaderCode());
  m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)] =
      CreateReSTIRComputePipeline(m_Allocator, m_PipelineLayout, GetSpatialShaderCode());
}

void ReSTIRPTRenderer::UpdateFrameDescriptors(const RenderInput& input)
{
  // The frame cycle index picks the descriptor set that is safe to rewrite for
  // the current frame in flight.
  const uint32_t frameSetIndex       = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  const uint32_t currentHistoryIndex = m_Context.GetCurrentHistoryIndex();
  const uint32_t previousHistoryIndex = m_Context.GetPreviousHistoryIndex();

  VkDescriptorImageInfo outputImageInfo = input.gBuffers->getDescriptorImageInfo(input.renderedImageIndex);
  outputImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo accumulationImageInfo = m_Resources.GetAccumulationImage().descriptor;
  accumulationImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo motionVectorsImageInfo = m_DenoiserResources.GetMotionVectorsImage().descriptor;
  motionVectorsImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo normalRoughnessImageInfo = m_DenoiserResources.GetNormalRoughnessImage().descriptor;
  normalRoughnessImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo baseColorMetalnessImageInfo = m_DenoiserResources.GetBaseColorMetalnessImage().descriptor;
  baseColorMetalnessImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo viewZImageInfo = m_DenoiserResources.GetViewZImage().descriptor;
  viewZImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo diffuseRadianceHitDistanceImageInfo = m_DenoiserResources.GetDiffuseRadianceHitDistanceImage().descriptor;
  diffuseRadianceHitDistanceImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo specularRadianceHitDistanceImageInfo = m_DenoiserResources.GetSpecularRadianceHitDistanceImage().descriptor;
  specularRadianceHitDistanceImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  // History buffer bindings are rotated here so the shaders always see
  // "current" and "previous" through stable binding numbers.
  std::array<VkDescriptorBufferInfo, 8> bufferInfos{
      VkDescriptorBufferInfo{m_Resources.GetInitialReservoirBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetScratchReservoirBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetHistoryReservoirBuffer(currentHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetHistoryReservoirBuffer(previousHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetSurfaceBuffer(currentHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetSurfaceBuffer(previousHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetNeighborOffsetBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetDebugBuffer().buffer, 0, VK_WHOLE_SIZE},
  };

  VkAccelerationStructureKHR accel = input.topLevelAS->accel;
  VkWriteDescriptorSetAccelerationStructureKHR accelerationInfo{
      .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
      .accelerationStructureCount = 1,
      .pAccelerationStructures    = &accel,
  };

  std::array<VkWriteDescriptorSet, 17> writes{};
  uint32_t                             writeCount = 0;

  writes[writeCount]       = m_DescPack.makeWrite(shaderio::ReSTIRBindingPoints::eReSTIRTlas, frameSetIndex);
  writes[writeCount].pNext = &accelerationInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.makeWrite(shaderio::ReSTIRBindingPoints::eReSTIROutputImage, frameSetIndex);
  writes[writeCount].pImageInfo = &outputImageInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.makeWrite(shaderio::ReSTIRBindingPoints::eReSTIRAccumulationImage, frameSetIndex);
  writes[writeCount].pImageInfo = &accumulationImageInfo;
  ++writeCount;

  const std::array<uint32_t, 8> bufferBindings{
      shaderio::ReSTIRBindingPoints::eReSTIRInitialReservoirBuffer,
      shaderio::ReSTIRBindingPoints::eReSTIRScratchReservoirBuffer,
      shaderio::ReSTIRBindingPoints::eReSTIRCurrentHistoryReservoirBuffer,
      shaderio::ReSTIRBindingPoints::eReSTIRPreviousHistoryReservoirBuffer,
      shaderio::ReSTIRBindingPoints::eReSTIRCurrentSurfaceBuffer,
      shaderio::ReSTIRBindingPoints::eReSTIRPreviousSurfaceBuffer,
      shaderio::ReSTIRBindingPoints::eReSTIRNeighborOffsetBuffer,
      shaderio::ReSTIRBindingPoints::eReSTIRDebugBuffer,
  };

  for(size_t i = 0; i < bufferBindings.size(); ++i)
  {
    writes[writeCount]             = m_DescPack.makeWrite(bufferBindings[i], frameSetIndex);
    writes[writeCount].pBufferInfo = &bufferInfos[i];
    ++writeCount;
  }

  const std::array<uint32_t, 6> imageBindings{
      shaderio::ReSTIRBindingPoints::eReSTIRMotionVectorsImage,
      shaderio::ReSTIRBindingPoints::eReSTIRNormalRoughnessImage,
      shaderio::ReSTIRBindingPoints::eReSTIRBaseColorMetalnessImage,
      shaderio::ReSTIRBindingPoints::eReSTIRViewZImage,
      shaderio::ReSTIRBindingPoints::eReSTIRDiffuseRadianceHitDistanceImage,
      shaderio::ReSTIRBindingPoints::eReSTIRSpecularRadianceHitDistanceImage,
  };
  const std::array<VkDescriptorImageInfo*, 6> imageInfos{
      &motionVectorsImageInfo,
      &normalRoughnessImageInfo,
      &baseColorMetalnessImageInfo,
      &viewZImageInfo,
      &diffuseRadianceHitDistanceImageInfo,
      &specularRadianceHitDistanceImageInfo,
  };
  for(size_t i = 0; i < imageBindings.size(); ++i)
  {
    writes[writeCount]            = m_DescPack.makeWrite(imageBindings[i], frameSetIndex);
    writes[writeCount].pImageInfo = imageInfos[i];
    ++writeCount;
  }

  vkUpdateDescriptorSets(m_Allocator->getDevice(), writeCount, writes.data(), 0, nullptr);
}

void ReSTIRPTRenderer::RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  shaderio::ReSTIRPTPushConstant passPushConstant = pushConstant;
  // The same shader code uses this tag to distinguish initial path generation
  // from final replay.
  passPushConstant.pathTraceInvocationType        = shaderio::eReSTIRPTPathTraceInvocationTypeInitial;
  TraceReSTIRRayTracingPass(input.cmd, m_InitialSamplingPass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                            kReSTIRPTPushConstantStages, passPushConstant, input.gBuffers->getSize());
}

void ReSTIRPTRenderer::RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  DispatchReSTIRComputePass(input.cmd, m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)], m_PipelineLayout,
                            *m_DescPack.getSetPtr(frameSetIndex), kReSTIRPTPushConstantStages, pushConstant, input.gBuffers->getSize(),
                            kComputeGroupSize);
}

void ReSTIRPTRenderer::RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  DispatchReSTIRComputePass(input.cmd, m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)], m_PipelineLayout,
                            *m_DescPack.getSetPtr(frameSetIndex), kReSTIRPTPushConstantStages, pushConstant, input.gBuffers->getSize(),
                            kComputeGroupSize);
}

void ReSTIRPTRenderer::RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  shaderio::ReSTIRPTPushConstant passPushConstant = pushConstant;
  // Final replay traces the continuation behind the selected reservoir sample.
  passPushConstant.pathTraceInvocationType        = shaderio::eReSTIRPTPathTraceInvocationTypeReplay;
  TraceReSTIRRayTracingPass(input.cmd, m_FinalShadingPass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                            kReSTIRPTPushConstantStages, passPushConstant, input.gBuffers->getSize());
}

}  // namespace nvsamples
