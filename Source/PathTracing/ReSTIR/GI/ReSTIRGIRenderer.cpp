#include "ReSTIRGIRenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include <nvapp/application.hpp>
#include <nvvk/barriers.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

#include "Common/Utils.hpp"
#include "ReSTIR/GI/ReSTIRGI.h"

#include "_autogen/ReSTIRGIFinalShading.slang.h"
#include "_autogen/ReSTIRGIGenerateInitialSamples.slang.h"
#include "_autogen/ReSTIRGISpatialResampling.slang.h"
#include "_autogen/ReSTIRGITemporalResampling.slang.h"

namespace nvsamples
{

namespace
{

constexpr uint32_t kComputeGroupSize = 8;
constexpr uint32_t kRequestedMaxBounces = 8;
constexpr VkShaderStageFlags kReSTIRGIPushConstantStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR
                                                           | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR
                                                           | VK_SHADER_STAGE_COMPUTE_BIT;

VkShaderModuleCreateInfo GetInitialSamplingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRGIGenerateInitialSamples_slang));
}

VkShaderModuleCreateInfo GetTemporalShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRGITemporalResampling_slang));
}

VkShaderModuleCreateInfo GetSpatialShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRGISpatialResampling_slang));
}

VkShaderModuleCreateInfo GetFinalShadingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(ReSTIRGIFinalShading_slang));
}

restir::ReSTIRGI_ResamplingMode MapResamplingMode(ReSTIRResamplingMode mode)
{
  switch(mode)
  {
    case ReSTIRResamplingMode::eTemporal:
      return restir::ReSTIRGI_ResamplingMode::Temporal;
    case ReSTIRResamplingMode::eSpatial:
      return restir::ReSTIRGI_ResamplingMode::Spatial;
    case ReSTIRResamplingMode::eTemporalAndSpatial:
      return restir::ReSTIRGI_ResamplingMode::TemporalAndSpatial;
    case ReSTIRResamplingMode::eNone:
    default:
      return restir::ReSTIRGI_ResamplingMode::None;
  }
}

}  // namespace

ReSTIRGIRenderer::ReSTIRGIRenderer(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
    , m_Resources(ReSTIRGIResources::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
{
}

ReSTIRGIRenderer::~ReSTIRGIRenderer() = default;

void ReSTIRGIRenderer::Initialize()
{
  if(m_App == nullptr || m_Allocator == nullptr || m_MaxTextureDescriptors == 0)
  {
    return;
  }

  QueryRayTracingProperties();
  CreateDescriptorSetLayout();
  CreateParameterBuffers();
  CreatePipelineLayout();
  CreateInitialSamplingPipeline();
  CreateFinalShadingPipeline();
  CreateComputePipelines();
  InvalidateHistory();
}

void ReSTIRGIRenderer::Destroy()
{
  if(m_Allocator == nullptr)
  {
    return;
  }

  VkDevice device = m_Allocator->getDevice();
  m_Resources.Destroy();
  DestroyReSTIRRayTracingPass(m_Allocator, m_InitialSamplingPass);
  DestroyReSTIRRayTracingPass(m_Allocator, m_FinalShadingPass);

  for(VkPipeline& computePipeline : m_ComputePipelines)
  {
    vkDestroyPipeline(device, computePipeline, nullptr);
    computePipeline = VK_NULL_HANDLE;
  }

  for(nvvk::Buffer& parameterBuffer : m_ParameterBuffers)
  {
    m_Allocator->destroyBuffer(parameterBuffer);
    parameterBuffer = {};
  }
  m_ParameterBuffers.clear();

  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
  m_PipelineLayout = VK_NULL_HANDLE;

  m_DescPack.deinit();
  m_GiContext.reset();
  m_AccumulatedFrames   = 0;
  m_HistoryInvalidated  = true;
  m_HasHistorySignature = false;
  m_NeedsHistoryClear   = true;
}

bool ReSTIRGIRenderer::IsReady() const
{
  return m_PipelineLayout != VK_NULL_HANDLE && IsReSTIRRayTracingPassReady(m_InitialSamplingPass) && IsReSTIRRayTracingPassReady(m_FinalShadingPass)
         && m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)] != VK_NULL_HANDLE
         && m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)] != VK_NULL_HANDLE;
}

ReSTIRGISettings& ReSTIRGIRenderer::GetSettings()
{
  return m_Settings;
}

const ReSTIRGISettings& ReSTIRGIRenderer::GetSettings() const
{
  return m_Settings;
}

uint32_t ReSTIRGIRenderer::GetAccumulatedFrameCount() const
{
  return m_Settings.common.accumulate ? m_AccumulatedFrames : 0u;
}

uint32_t ReSTIRGIRenderer::GetPipelineBounceLimit() const
{
  return m_PipelineBounceLimit;
}

void ReSTIRGIRenderer::InvalidateHistory()
{
  m_AccumulatedFrames   = 0;
  m_HistoryInvalidated  = true;
  m_HasHistorySignature = false;
  m_NeedsHistoryClear   = true;
  m_Context.InvalidateHistory();
  m_GiContext.reset();
}

nvvk::DescriptorPack& ReSTIRGIRenderer::GetDescriptorPack()
{
  return m_DescPack;
}

const nvvk::DescriptorPack& ReSTIRGIRenderer::GetDescriptorPack() const
{
  return m_DescPack;
}

void ReSTIRGIRenderer::Render(const RenderInput& input)
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
  EnsureGIContext(viewportSize);

  const HistorySignature currentSignature = MakeReSTIRHistorySignature(*input.sceneInfo, input.topLevelAS->address, viewportSize);
  const bool signatureChanged = !m_HasHistorySignature
                                || std::memcmp(&currentSignature, &m_LastHistorySignature, sizeof(HistorySignature)) != 0;
  const bool historyInvalidated = m_HistoryInvalidated || (m_Settings.common.accumulate && signatureChanged);
  if(historyInvalidated)
  {
    m_AccumulatedFrames = 0;
    m_Context.InvalidateHistory();
    m_GiContext.reset();
    EnsureGIContext(viewportSize);
    m_NeedsHistoryClear = true;
  }

  m_GiContext->SetFrameIndex(m_Context.GetFrameIndex());
  m_GiContext->SetResamplingMode(MapResamplingMode(m_Settings.common.resamplingMode));
  m_GiContext->SetTemporalResamplingParameters(m_Settings.temporalResampling);
  m_GiContext->SetSpatialResamplingParameters(m_Settings.spatialResampling);
  m_GiContext->SetShadingParameters(m_Settings.shading);
  m_Settings.continuationMaxBounces = std::min(m_Settings.continuationMaxBounces, m_PipelineBounceLimit);

  const uint32_t frameSetIndex = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_ParameterBuffers.size());
  const shaderio::ReSTIRGIParameters parameters{
      .runtimeParams         = m_GiContext->GetRuntimeParameters(),
      .reservoirBufferParams = m_GiContext->GetReservoirBufferParameters(),
      .bufferIndices         = m_GiContext->GetBufferIndices(),
      .temporalResampling    = m_GiContext->GetTemporalResamplingParameters(),
      .spatialResampling     = m_GiContext->GetSpatialResamplingParameters(),
      .boilingFilter         = m_Settings.common.boilingFilter,
      .shading               = m_GiContext->GetShadingParameters(),
  };
  UpdateParameterBuffer(frameSetIndex, parameters);
  UpdateFrameDescriptors(input);
  TransitionReSTIRStorageImages(input.cmd, const_cast<nvvk::Image&>(m_Resources.GetAccumulationImage()),
                                input.gBuffers->getColorImage(input.renderedImageIndex),
                                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  if(m_NeedsHistoryClear)
  {
    ClearHistoryBuffers(input.cmd);
    m_NeedsHistoryClear = false;
  }

  const shaderio::ReSTIRGIPushConstant pushConstant{
      .sceneInfoAddress       = (shaderio::GltfSceneInfo*)input.sceneResource->bSceneInfo.address,
      .accumulatedFrames      = m_Settings.common.accumulate ? m_AccumulatedFrames : 0u,
      .flags                  = m_Settings.common.accumulate ? shaderio::eReSTIRFlagAccumulate : 0u,
      .continuationMaxBounces = m_Settings.continuationMaxBounces,
      .debugView              = static_cast<uint32_t>(m_Settings.common.debugView),
  };

  RunInitialSamplingPass(input, pushConstant);

  const bool enableTemporal = IsReSTIRTemporalResamplingEnabled(m_Settings.common.resamplingMode);
  const bool enableSpatial  = IsReSTIRSpatialResamplingEnabled(m_Settings.common.resamplingMode);

  VkPipelineStageFlags2 lastStage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
  if(enableTemporal)
  {
    nvvk::cmdMemoryBarrier(input.cmd, lastStage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    RunTemporalPass(input, pushConstant);
    lastStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  }

  if(enableSpatial)
  {
    nvvk::cmdMemoryBarrier(input.cmd, lastStage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    RunSpatialPass(input, pushConstant);
    lastStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  }

  nvvk::cmdMemoryBarrier(input.cmd, lastStage, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  RunFinalShadingPass(input, pushConstant);
  nvvk::cmdMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  m_LastHistorySignature = currentSignature;
  m_HasHistorySignature  = true;
  m_HistoryInvalidated   = false;
  m_Context.AdvanceFrame();
  m_AccumulatedFrames = m_Settings.common.accumulate ? (m_AccumulatedFrames + 1u) : 0u;
}

void ReSTIRGIRenderer::QueryRayTracingProperties()
{
  VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m_RtProperties};
  vkGetPhysicalDeviceProperties2(m_Allocator->getPhysicalDevice(), &props);
  m_MaxBounceLimit                  = (m_RtProperties.maxRayRecursionDepth > 0) ? (m_RtProperties.maxRayRecursionDepth - 1u) : 0u;
  m_PipelineBounceLimit             = std::min(kRequestedMaxBounces, m_MaxBounceLimit);
  m_Settings.continuationMaxBounces = std::min(m_Settings.continuationMaxBounces, m_PipelineBounceLimit);
}

void ReSTIRGIRenderer::CreateDescriptorSetLayout()
{
  const VkShaderStageFlags allStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR
                                       | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR
                                       | VK_SHADER_STAGE_COMPUTE_BIT;

  nvvk::DescriptorBindings bindings;
  bindings.addBinding({.binding = shaderio::ReSTIRGIBindingPoints::eReSTIRGITextures,
                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       .descriptorCount = m_MaxTextureDescriptors,
                       .stageFlags = allStages},
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                          | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGITlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGIOutputImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGIAccumulationImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGIReservoirBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGIInitialSampleBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGICurrentSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGIPreviousSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGINeighborOffsetBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGIParamsBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRGIBindingPoints::eReSTIRGIDebugBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);

  m_DescPack.init(bindings, m_Allocator->getDevice(), m_App->getFrameCycleSize(), VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                  VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
}

void ReSTIRGIRenderer::CreatePipelineLayout()
{
  const VkPushConstantRange pushConstantRange{
      .stageFlags = kReSTIRGIPushConstantStages,
      .offset     = 0,
      .size       = sizeof(shaderio::ReSTIRGIPushConstant),
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

void ReSTIRGIRenderer::CreateParameterBuffers()
{
  const uint32_t frameSetCount = std::max(1u, m_App->getFrameCycleSize());
  m_ParameterBuffers.resize(frameSetCount);

  for(nvvk::Buffer& parameterBuffer : m_ParameterBuffers)
  {
    NVVK_CHECK(m_Allocator->createBuffer(
        parameterBuffer, sizeof(shaderio::ReSTIRGIParameters), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT));
    NVVK_DBG_NAME(parameterBuffer.buffer);
  }
}

void ReSTIRGIRenderer::CreateInitialSamplingPipeline()
{
  CreateReSTIRRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetInitialSamplingShaderCode(), 2u, m_InitialSamplingPass);
}

void ReSTIRGIRenderer::CreateFinalShadingPipeline()
{
  CreateReSTIRRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetFinalShadingShaderCode(),
                             std::max(1u, m_PipelineBounceLimit + 1u), m_FinalShadingPass);
}

void ReSTIRGIRenderer::CreateComputePipelines()
{
  m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)] =
      CreateReSTIRComputePipeline(m_Allocator, m_PipelineLayout, GetTemporalShaderCode());
  m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)] =
      CreateReSTIRComputePipeline(m_Allocator, m_PipelineLayout, GetSpatialShaderCode());
}

void ReSTIRGIRenderer::EnsureGIContext(VkExtent2D viewportSize)
{
  if(m_GiContext != nullptr)
  {
    const restir::ReSTIRGIStaticParameters& staticParameters = m_GiContext->GetStaticParameters();
    if(staticParameters.RenderWidth == viewportSize.width && staticParameters.RenderHeight == viewportSize.height
       && staticParameters.NeighborOffsetCount == m_Resources.GetNeighborOffsetCount())
    {
      return;
    }
  }

  const restir::ReSTIRGIStaticParameters staticParameters{
      .NeighborOffsetCount      = m_Resources.GetNeighborOffsetCount(),
      .RenderWidth              = viewportSize.width,
      .RenderHeight             = viewportSize.height,
      .CheckerboardSamplingMode = restir::CheckerboardMode::Off,
  };
  m_GiContext = std::make_unique<restir::ReSTIRGIContext>(staticParameters);
  m_NeedsHistoryClear = true;
}

void ReSTIRGIRenderer::UpdateFrameDescriptors(const RenderInput& input)
{
  const uint32_t frameSetIndex        = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  const uint32_t currentHistoryIndex  = m_Context.GetCurrentHistoryIndex();
  const uint32_t previousHistoryIndex = m_Context.GetPreviousHistoryIndex();

  VkDescriptorImageInfo outputImageInfo = input.gBuffers->getDescriptorImageInfo(input.renderedImageIndex);
  outputImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo accumulationImageInfo = m_Resources.GetAccumulationImage().descriptor;
  accumulationImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  std::array<VkDescriptorBufferInfo, 7> bufferInfos{
      VkDescriptorBufferInfo{m_Resources.GetReservoirBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetInitialSampleBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetSurfaceBuffer(currentHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetSurfaceBuffer(previousHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetNeighborOffsetBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_ParameterBuffers[frameSetIndex].buffer, 0, sizeof(shaderio::ReSTIRGIParameters)},
      VkDescriptorBufferInfo{m_Resources.GetDebugBuffer().buffer, 0, VK_WHOLE_SIZE},
  };

  VkAccelerationStructureKHR accel = input.topLevelAS->accel;
  VkWriteDescriptorSetAccelerationStructureKHR accelerationInfo{
      .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
      .accelerationStructureCount = 1,
      .pAccelerationStructures    = &accel,
  };

  std::array<VkWriteDescriptorSet, 10> writes{};
  uint32_t                            writeCount = 0;

  writes[writeCount]       = m_DescPack.makeWrite(shaderio::ReSTIRGIBindingPoints::eReSTIRGITlas, frameSetIndex);
  writes[writeCount].pNext = &accelerationInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.makeWrite(shaderio::ReSTIRGIBindingPoints::eReSTIRGIOutputImage, frameSetIndex);
  writes[writeCount].pImageInfo = &outputImageInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.makeWrite(shaderio::ReSTIRGIBindingPoints::eReSTIRGIAccumulationImage, frameSetIndex);
  writes[writeCount].pImageInfo = &accumulationImageInfo;
  ++writeCount;

  const std::array<uint32_t, 5> bufferBindings{
      shaderio::ReSTIRGIBindingPoints::eReSTIRGIReservoirBuffer,
      shaderio::ReSTIRGIBindingPoints::eReSTIRGIInitialSampleBuffer,
      shaderio::ReSTIRGIBindingPoints::eReSTIRGICurrentSurfaceBuffer,
      shaderio::ReSTIRGIBindingPoints::eReSTIRGIPreviousSurfaceBuffer,
      shaderio::ReSTIRGIBindingPoints::eReSTIRGINeighborOffsetBuffer,
  };

  for(size_t i = 0; i < bufferBindings.size(); ++i)
  {
    writes[writeCount]             = m_DescPack.makeWrite(bufferBindings[i], frameSetIndex);
    writes[writeCount].pBufferInfo = &bufferInfos[i];
    ++writeCount;
  }

  writes[writeCount]             = m_DescPack.makeWrite(shaderio::ReSTIRGIBindingPoints::eReSTIRGIParamsBuffer, frameSetIndex);
  writes[writeCount].pBufferInfo = &bufferInfos[5];
  ++writeCount;

  writes[writeCount]             = m_DescPack.makeWrite(shaderio::ReSTIRGIBindingPoints::eReSTIRGIDebugBuffer, frameSetIndex);
  writes[writeCount].pBufferInfo = &bufferInfos[6];
  ++writeCount;

  vkUpdateDescriptorSets(m_Allocator->getDevice(), writeCount, writes.data(), 0, nullptr);
}

void ReSTIRGIRenderer::ClearHistoryBuffers(VkCommandBuffer cmd)
{
  vkCmdFillBuffer(cmd, m_Resources.GetReservoirBuffer().buffer, 0, m_Resources.GetReservoirBuffer().bufferSize, 0);
  vkCmdFillBuffer(cmd, m_Resources.GetInitialSampleBuffer().buffer, 0, m_Resources.GetInitialSampleBuffer().bufferSize, 0);
  vkCmdFillBuffer(cmd, m_Resources.GetSurfaceBuffer(0).buffer, 0, m_Resources.GetSurfaceBuffer(0).bufferSize, 0);
  vkCmdFillBuffer(cmd, m_Resources.GetSurfaceBuffer(1).buffer, 0, m_Resources.GetSurfaceBuffer(1).bufferSize, 0);
  vkCmdFillBuffer(cmd, m_Resources.GetDebugBuffer().buffer, 0, m_Resources.GetDebugBuffer().bufferSize, 0);

  const VkBufferMemoryBarrier2 barriers[] = {
      VkBufferMemoryBarrier2{
          .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          .buffer        = m_Resources.GetReservoirBuffer().buffer,
          .offset        = 0,
          .size          = m_Resources.GetReservoirBuffer().bufferSize,
      },
      VkBufferMemoryBarrier2{
          .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          .buffer        = m_Resources.GetInitialSampleBuffer().buffer,
          .offset        = 0,
          .size          = m_Resources.GetInitialSampleBuffer().bufferSize,
      },
      VkBufferMemoryBarrier2{
          .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          .buffer        = m_Resources.GetSurfaceBuffer(0).buffer,
          .offset        = 0,
          .size          = m_Resources.GetSurfaceBuffer(0).bufferSize,
      },
      VkBufferMemoryBarrier2{
          .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          .buffer        = m_Resources.GetSurfaceBuffer(1).buffer,
          .offset        = 0,
          .size          = m_Resources.GetSurfaceBuffer(1).bufferSize,
      },
      VkBufferMemoryBarrier2{
          .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          .buffer        = m_Resources.GetDebugBuffer().buffer,
          .offset        = 0,
          .size          = m_Resources.GetDebugBuffer().bufferSize,
      },
  };

  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = uint32_t(std::size(barriers)),
      .pBufferMemoryBarriers    = barriers,
  };
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

void ReSTIRGIRenderer::RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRGIPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  TraceReSTIRRayTracingPass(input.cmd, m_InitialSamplingPass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                            kReSTIRGIPushConstantStages, pushConstant, input.gBuffers->getSize());
}

void ReSTIRGIRenderer::RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRGIPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  DispatchReSTIRComputePass(input.cmd, m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)], m_PipelineLayout,
                            *m_DescPack.getSetPtr(frameSetIndex), kReSTIRGIPushConstantStages, pushConstant, input.gBuffers->getSize(),
                            kComputeGroupSize);
}

void ReSTIRGIRenderer::RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRGIPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  DispatchReSTIRComputePass(input.cmd, m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)], m_PipelineLayout,
                            *m_DescPack.getSetPtr(frameSetIndex), kReSTIRGIPushConstantStages, pushConstant, input.gBuffers->getSize(),
                            kComputeGroupSize);
}

void ReSTIRGIRenderer::RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRGIPushConstant& pushConstant)
{
  const uint32_t frameSetIndex = GetReSTIRFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  TraceReSTIRRayTracingPass(input.cmd, m_FinalShadingPass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                            kReSTIRGIPushConstantStages, pushConstant, input.gBuffers->getSize());
}

void ReSTIRGIRenderer::UpdateParameterBuffer(uint32_t frameSetIndex, const shaderio::ReSTIRGIParameters& parameters)
{
  if(frameSetIndex >= m_ParameterBuffers.size())
  {
    return;
  }

  nvvk::Buffer& parameterBuffer = m_ParameterBuffers[frameSetIndex];
  std::memcpy(parameterBuffer.mapping, &parameters, sizeof(parameters));
  NVVK_CHECK(m_Allocator->flushBuffer(parameterBuffer, 0, sizeof(parameters)));
}

}  // namespace nvsamples
