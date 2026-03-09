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

uint32_t DivideRoundUp(uint32_t value, uint32_t divisor)
{
  return (value + divisor - 1u) / divisor;
}

}  // namespace

ReSTIRPTRenderer::ReSTIRPTRenderer(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
    , m_Resources(ReSTIRResources::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
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
  InvalidateHistory();
}

void ReSTIRPTRenderer::Destroy()
{
  if(m_Allocator == nullptr)
  {
    return;
  }

  VkDevice device = m_Allocator->getDevice();
  m_Resources.Destroy();

  DestroyRayTracingPass(m_InitialSamplingPass);
  DestroyRayTracingPass(m_FinalShadingPass);

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
}

bool ReSTIRPTRenderer::IsReady() const
{
  return m_PipelineLayout != VK_NULL_HANDLE && m_InitialSamplingPass.pipeline != VK_NULL_HANDLE
         && m_InitialSamplingPass.sbtBuffer.buffer != VK_NULL_HANDLE && m_FinalShadingPass.pipeline != VK_NULL_HANDLE
         && m_FinalShadingPass.sbtBuffer.buffer != VK_NULL_HANDLE
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
  return m_Settings.common.accumulate ? m_AccumulatedFrames : 0;
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
  m_Context.InvalidateHistory();
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

  // Camera-driven invalidation is only needed when final accumulation is
  // active. Temporal ReSTIR reuse is allowed to follow normal camera motion.
  const HistorySignature currentSignature = MakeHistorySignature(input, viewportSize);
  const bool signatureChanged = !m_HasHistorySignature
                                || std::memcmp(&currentSignature, &m_LastHistorySignature, sizeof(HistorySignature)) != 0;
  const bool historyInvalidated = m_HistoryInvalidated || (m_Settings.common.accumulate && signatureChanged);
  if(historyInvalidated)
  {
    m_AccumulatedFrames = 0;
    m_Context.InvalidateHistory();
  }

  UpdateFrameDescriptors(input);
  // The passes write storage images and storage buffers, so the output images
  // must be in GENERAL before the first dispatch.
  TransitionStorageImages(input.cmd, input);

  const bool enableTemporal = m_Settings.common.resamplingMode == ReSTIRResamplingMode::eTemporal
                              || m_Settings.common.resamplingMode == ReSTIRResamplingMode::eTemporalAndSpatial;
  const bool enableSpatial  = m_Settings.common.resamplingMode == ReSTIRResamplingMode::eSpatial
                              || m_Settings.common.resamplingMode == ReSTIRResamplingMode::eTemporalAndSpatial;

  shaderio::ReSTIRInitialSamplingParameters initialSampling = m_Settings.common.initialSampling;
  initialSampling.maxBounceDepth = std::min(initialSampling.maxBounceDepth, m_PipelineBounceLimit);

  const shaderio::ReSTIRPTPushConstant pushConstant{
      .sceneInfoAddress            = (shaderio::GltfSceneInfo*)input.sceneResource->bSceneInfo.address,
      .rngFrameNumber              = m_Context.GetFrameIndex(),
      .accumulatedFrames           = m_Settings.common.accumulate ? m_AccumulatedFrames : 0u,
      .flags                       = BuildFrameFlags(enableTemporal, enableSpatial),
      .initialSampling             = initialSampling,
      .temporalResampling          = m_Settings.common.temporalResampling,
      .spatialResampling           = m_Settings.common.spatialResampling,
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

  m_LastHistorySignature = currentSignature;
  m_HasHistorySignature  = true;
  m_HistoryInvalidated   = false;
  m_Context.AdvanceFrame();
  m_AccumulatedFrames = m_Settings.common.accumulate ? (m_AccumulatedFrames + 1u) : 0u;
}

uint32_t ReSTIRPTRenderer::BuildFrameFlags(bool enableTemporal, bool enableSpatial) const
{
  uint32_t flags = 0;
  if(m_Settings.common.accumulate)
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
  CreateRayTracingPass(GetInitialSamplingShaderCode(), m_InitialSamplingPass, "ReSTIRPTInitialSamplingPipeline");
}

void ReSTIRPTRenderer::CreateFinalShadingPipeline()
{
  CreateRayTracingPass(GetFinalShadingShaderCode(), m_FinalShadingPass, "ReSTIRPTFinalShadingPipeline");
}

void ReSTIRPTRenderer::CreateComputePipelines()
{
  // Temporal and spatial passes share the same descriptor layout but use
  // different entry points and dispatch logic.
  m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)] =
      CreateComputePipeline(GetTemporalShaderCode(), "ReSTIRPTTemporalPipeline");
  m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)] =
      CreateComputePipeline(GetSpatialShaderCode(), "ReSTIRPTSpatialPipeline");
}

void ReSTIRPTRenderer::CreateRayTracingPass(const VkShaderModuleCreateInfo& shaderCode,
                                            RayTracingPassState&            passState,
                                            const char*                     debugName)
{
  (void)debugName;

  VkDevice device = m_Allocator->getDevice();

  VkShaderModule shaderModule = VK_NULL_HANDLE;
  NVVK_CHECK(vkCreateShaderModule(device, &shaderCode, nullptr, &shaderModule));

  // Both RT passes reuse the same shader layout: raygen, two miss shaders, one
  // shaded hit group, and one shadow hit group.
  enum ShaderStageIndex : uint32_t
  {
    eRaygen = 0,
    eMiss,
    eShadowMiss,
    eAnyHit,
    eShadowAnyHit,
    eClosestHit,
    eStageCount,
  };

  std::array<VkPipelineShaderStageCreateInfo, eStageCount> stages{};
  for(VkPipelineShaderStageCreateInfo& stage : stages)
  {
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.module = shaderModule;
  }

  stages[eRaygen].stage       = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  stages[eRaygen].pName       = "rgenMain";
  stages[eMiss].stage         = VK_SHADER_STAGE_MISS_BIT_KHR;
  stages[eMiss].pName         = "rmissMain";
  stages[eShadowMiss].stage   = VK_SHADER_STAGE_MISS_BIT_KHR;
  stages[eShadowMiss].pName   = "shadowMissMain";
  stages[eAnyHit].stage       = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
  stages[eAnyHit].pName       = "rahitMain";
  stages[eShadowAnyHit].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
  stages[eShadowAnyHit].pName = "shadowAnyHitMain";
  stages[eClosestHit].stage   = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
  stages[eClosestHit].pName   = "rchitMain";

  VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
  group.anyHitShader       = VK_SHADER_UNUSED_KHR;
  group.closestHitShader   = VK_SHADER_UNUSED_KHR;
  group.generalShader      = VK_SHADER_UNUSED_KHR;
  group.intersectionShader = VK_SHADER_UNUSED_KHR;

  // The shadow path only needs any-hit masking, so its hit group omits a
  // closest-hit shader entirely.
  std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
  groups.reserve(5);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eRaygen;
  groups.push_back(group);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eMiss;
  groups.push_back(group);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eShadowMiss;
  groups.push_back(group);

  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.anyHitShader     = eAnyHit;
  group.closestHitShader = eClosestHit;
  groups.push_back(group);

  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.anyHitShader     = eShadowAnyHit;
  group.closestHitShader = VK_SHADER_UNUSED_KHR;
  groups.push_back(group);

  const VkRayTracingPipelineCreateInfoKHR pipelineInfo{
      .sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
      .stageCount                   = static_cast<uint32_t>(stages.size()),
      .pStages                      = stages.data(),
      .groupCount                   = static_cast<uint32_t>(groups.size()),
      .pGroups                      = groups.data(),
      .maxPipelineRayRecursionDepth = std::max(1u, m_PipelineBounceLimit + 1u),
      .layout                       = m_PipelineLayout,
  };

  NVVK_CHECK(vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &passState.pipeline));

  passState.sbtGenerator.init(device, m_RtProperties);
  // The shader binding table is pass-local because the two RT passes use
  // different pipelines, even though they share the same layout.
  const size_t sbtBufferSize = passState.sbtGenerator.calculateSBTBufferSize(passState.pipeline, pipelineInfo);
  NVVK_CHECK(m_Allocator->createBuffer(passState.sbtBuffer, sbtBufferSize,
                                       VK_BUFFER_USAGE_2_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                       VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                       passState.sbtGenerator.getBufferAlignment()));
  NVVK_CHECK(passState.sbtGenerator.populateSBTBuffer(passState.sbtBuffer.address, passState.sbtBuffer.bufferSize,
                                                      passState.sbtBuffer.mapping));
  passState.sbtRegions = passState.sbtGenerator.getSBTRegions(0);

  vkDestroyShaderModule(device, shaderModule, nullptr);
}

void ReSTIRPTRenderer::DestroyRayTracingPass(RayTracingPassState& passState)
{
  if(m_Allocator == nullptr)
  {
    return;
  }

  VkDevice device = m_Allocator->getDevice();
  m_Allocator->destroyBuffer(passState.sbtBuffer);
  passState.sbtBuffer = {};
  passState.sbtGenerator.deinit();
  passState.sbtRegions = {};
  vkDestroyPipeline(device, passState.pipeline, nullptr);
  passState.pipeline = VK_NULL_HANDLE;
}

void ReSTIRPTRenderer::UpdateFrameDescriptors(const RenderInput& input)
{
  // The frame cycle index picks the descriptor set that is safe to rewrite for
  // the current frame in flight.
  const uint32_t frameSetIndex       = std::min(m_App->getFrameCycleIndex(), uint32_t(m_DescPack.getSets().size() - 1));
  const uint32_t currentHistoryIndex = m_Context.GetCurrentHistoryIndex();
  const uint32_t previousHistoryIndex = m_Context.GetPreviousHistoryIndex();

  VkDescriptorImageInfo outputImageInfo = input.gBuffers->getDescriptorImageInfo(input.renderedImageIndex);
  outputImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo accumulationImageInfo = m_Resources.GetAccumulationImage().descriptor;
  accumulationImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

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

  std::array<VkWriteDescriptorSet, 11> writes{};
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

  vkUpdateDescriptorSets(m_Allocator->getDevice(), writeCount, writes.data(), 0, nullptr);
}

void ReSTIRPTRenderer::TransitionStorageImages(VkCommandBuffer cmd, const RenderInput& input)
{
  const VkPipelineStageFlags2 rtAndComputeStages = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

  // The accumulation image persists across frames, so its first transition can
  // start from UNDEFINED. The output image is treated as write-only here.
  std::array<VkImageMemoryBarrier2, 2> imageBarriers{};
  uint32_t                             imageBarrierCount = 0;

  if(m_Resources.GetAccumulationImage().descriptor.imageLayout != VK_IMAGE_LAYOUT_GENERAL)
  {
    imageBarriers[imageBarrierCount++] = VkImageMemoryBarrier2{
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask  = rtAndComputeStages,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout     = m_Resources.GetAccumulationImage().descriptor.imageLayout,
        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .image         = m_Resources.GetAccumulationImage().image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
    };
    const_cast<nvvk::Image&>(m_Resources.GetAccumulationImage()).descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  imageBarriers[imageBarrierCount++] = VkImageMemoryBarrier2{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask  = rtAndComputeStages,
      .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .image         = input.gBuffers->getColorImage(input.renderedImageIndex),
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
  };

  const VkDependencyInfo dependencyInfo{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = imageBarrierCount,
      .pImageMemoryBarriers    = imageBarriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

void ReSTIRPTRenderer::RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = std::min(m_App->getFrameCycleIndex(), uint32_t(m_DescPack.getSets().size() - 1));
  shaderio::ReSTIRPTPushConstant passPushConstant = pushConstant;
  // The same shader code uses this tag to distinguish initial path generation
  // from final replay.
  passPushConstant.pathTraceInvocationType        = shaderio::eReSTIRPTPathTraceInvocationTypeInitial;

  vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_InitialSamplingPass.pipeline);
  vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1, m_DescPack.getSetPtr(frameSetIndex), 0,
                          nullptr);
  vkCmdPushConstants(input.cmd, m_PipelineLayout, kReSTIRPTPushConstantStages, 0, sizeof(shaderio::ReSTIRPTPushConstant),
                     &passPushConstant);
  vkCmdTraceRaysKHR(input.cmd, &m_InitialSamplingPass.sbtRegions.raygen, &m_InitialSamplingPass.sbtRegions.miss,
                    &m_InitialSamplingPass.sbtRegions.hit, &m_InitialSamplingPass.sbtRegions.callable, input.gBuffers->getSize().width,
                    input.gBuffers->getSize().height, 1);
}

void ReSTIRPTRenderer::RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  DispatchComputePass(input.cmd, m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)], pushConstant, input.gBuffers->getSize());
}

void ReSTIRPTRenderer::RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  DispatchComputePass(input.cmd, m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)], pushConstant, input.gBuffers->getSize());
}

void ReSTIRPTRenderer::RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = std::min(m_App->getFrameCycleIndex(), uint32_t(m_DescPack.getSets().size() - 1));
  shaderio::ReSTIRPTPushConstant passPushConstant = pushConstant;
  // Final replay traces the continuation behind the selected reservoir sample.
  passPushConstant.pathTraceInvocationType        = shaderio::eReSTIRPTPathTraceInvocationTypeReplay;

  vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_FinalShadingPass.pipeline);
  vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1, m_DescPack.getSetPtr(frameSetIndex), 0,
                          nullptr);
  vkCmdPushConstants(input.cmd, m_PipelineLayout, kReSTIRPTPushConstantStages, 0, sizeof(shaderio::ReSTIRPTPushConstant), &passPushConstant);
  vkCmdTraceRaysKHR(input.cmd, &m_FinalShadingPass.sbtRegions.raygen, &m_FinalShadingPass.sbtRegions.miss, &m_FinalShadingPass.sbtRegions.hit,
                    &m_FinalShadingPass.sbtRegions.callable, input.gBuffers->getSize().width, input.gBuffers->getSize().height, 1);
}

void ReSTIRPTRenderer::DispatchComputePass(VkCommandBuffer cmd,
                                           VkPipeline      pipeline,
                                           const shaderio::ReSTIRPTPushConstant& pushConstant,
                                           VkExtent2D      viewportSize)
{
  const uint32_t frameSetIndex = std::min(m_App->getFrameCycleIndex(), uint32_t(m_DescPack.getSets().size() - 1));
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, m_DescPack.getSetPtr(frameSetIndex), 0, nullptr);
  vkCmdPushConstants(cmd, m_PipelineLayout, kReSTIRPTPushConstantStages, 0, sizeof(shaderio::ReSTIRPTPushConstant), &pushConstant);
  // Compute passes use one thread per pixel and derive the dispatch size from
  // the viewport dimensions.
  vkCmdDispatch(cmd, DivideRoundUp(viewportSize.width, kComputeGroupSize), DivideRoundUp(viewportSize.height, kComputeGroupSize), 1);
}

VkPipeline ReSTIRPTRenderer::CreateComputePipeline(const VkShaderModuleCreateInfo& shaderCode, const char* debugName) const
{
  (void)debugName;

  VkShaderModule shaderModule = VK_NULL_HANDLE;
  NVVK_CHECK(vkCreateShaderModule(m_Allocator->getDevice(), &shaderCode, nullptr, &shaderModule));

  const VkPipelineShaderStageCreateInfo shaderStage{
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shaderModule,
      .pName  = "main",
  };
  const VkComputePipelineCreateInfo pipelineInfo{
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = shaderStage,
      .layout = m_PipelineLayout,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  NVVK_CHECK(vkCreateComputePipelines(m_Allocator->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
  vkDestroyShaderModule(m_Allocator->getDevice(), shaderModule, nullptr);
  return pipeline;
}

ReSTIRPTRenderer::HistorySignature ReSTIRPTRenderer::MakeHistorySignature(const RenderInput& input, VkExtent2D viewportSize) const
{
  HistorySignature signature{};
  // Only values that affect reservoir validity belong here. Pure UI state that
  // does not change the estimator is intentionally excluded.
  signature.viewProjMatrix         = input.sceneInfo->viewProjMatrix;
  signature.projInvMatrix          = input.sceneInfo->projInvMatrix;
  signature.viewInvMatrix          = input.sceneInfo->viewInvMatrix;
  signature.cameraPosition         = input.sceneInfo->cameraPosition;
  signature.useSky                 = input.sceneInfo->useSky;
  signature.useHdrEnv              = input.sceneInfo->useHdrEnv;
  signature.environmentTextureIndex = input.sceneInfo->environmentTextureIndex;
  signature.backgroundColor        = input.sceneInfo->backgroundColor;
  signature.skySimpleParam         = input.sceneInfo->skySimpleParam;
  signature.topLevelAsAddress      = input.topLevelAS->address;
  signature.viewportSize           = viewportSize;
  return signature;
}

}  // namespace nvsamples
