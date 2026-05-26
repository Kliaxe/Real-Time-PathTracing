#include "ReSTIRDIRenderer.h"

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
#include "PathTracing/ReSTIR/ReSTIRDIParameterContext.h"

#include "_autogen/FinalShading.slang.h"
#include "_autogen/GenerateInitialSamples.slang.h"
#include "_autogen/SpatialResampling.slang.h"
#include "_autogen/TemporalResampling.slang.h"

namespace nvsamples
{

namespace
{

constexpr uint32_t kComputeGroupSize = 8;
constexpr uint32_t kRequestedMaxBounces = 8;
constexpr VkShaderStageFlags kReSTIRDIPushConstantStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR
                                                           | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR
                                                           | VK_SHADER_STAGE_COMPUTE_BIT;
static_assert(sizeof(shaderio::ReSTIRDIPushConstant) <= 256, "ReSTIR DI push constants must fit Vulkan's minimum 256-byte limit.");

void TransitionStorageImageForWrite(VkCommandBuffer cmd, nvvk::Image& image, VkPipelineStageFlags2 dstStageMask)
{
  if(image.image == VK_NULL_HANDLE)
  {
    return;
  }

  if(image.descriptor.imageLayout == VK_IMAGE_LAYOUT_GENERAL)
  {
    // The image is already in the right layout, but this still orders previous writes before this pass.
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
  // First use this frame moves the image into GENERAL so ray tracing shaders can write it.
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

VkShaderModuleCreateInfo GetInitialSamplingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(GenerateInitialSamples_slang));
}

VkShaderModuleCreateInfo GetTemporalShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(TemporalResampling_slang));
}

VkShaderModuleCreateInfo GetSpatialShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(SpatialResampling_slang));
}

VkShaderModuleCreateInfo GetFinalShadingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(FinalShading_slang));
}

}  // namespace

ReSTIRDIRenderer::ReSTIRDIRenderer(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
    , m_Resources(ReSTIRDIResources::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
    , m_DenoiserResources(DenoiserResources::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
    , m_NrdDenoiser(NrdDenoiser::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
{
}

ReSTIRDIRenderer::~ReSTIRDIRenderer() = default;

void ReSTIRDIRenderer::Initialize()
{
  if(m_App == nullptr || m_Allocator == nullptr || m_MaxTextureDescriptors == 0)
  {
    return;
  }

  // Vulkan objects are created once; viewport-sized buffers are created later when a frame arrives.
  QueryRayTracingProperties();
  CreateDescriptorSetLayout();
  CreateParameterBuffers();
  CreatePipelineLayout();
  CreateInitialSamplingPipeline();
  CreateFinalShadingPipeline();
  CreateComputePipelines();
  m_NrdDenoiser.Initialize();
  InvalidateHistory();
}

void ReSTIRDIRenderer::Destroy()
{
  if(m_Allocator == nullptr)
  {
    return;
  }

  VkDevice device = m_Allocator->getDevice();

  // Destroy dependents before descriptor/pipeline layout state they were built against.
  m_NrdDenoiser.Destroy();
  m_DenoiserResources.Destroy();
  m_Resources.Destroy();
  DestroyReSTIRDIRayTracingPass(m_Allocator, m_InitialSamplingPass);
  DestroyReSTIRDIRayTracingPass(m_Allocator, m_FinalShadingPass);

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
  m_ParameterContext.reset();
  m_AccumulatedFrames   = 0;
  m_HistoryInvalidated  = true;
  m_HasAccumulationSignature = false;
  m_HasDenoiserSignature = false;
  m_NeedsHistoryClear   = true;
}

bool ReSTIRDIRenderer::IsReady() const
{
  return m_PipelineLayout != VK_NULL_HANDLE && IsReSTIRDIRayTracingPassReady(m_InitialSamplingPass) && IsReSTIRDIRayTracingPassReady(m_FinalShadingPass)
         && m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)] != VK_NULL_HANDLE
         && m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)] != VK_NULL_HANDLE;
}

ReSTIRDISettings& ReSTIRDIRenderer::GetSettings()
{
  return m_Settings;
}

const ReSTIRDISettings& ReSTIRDIRenderer::GetSettings() const
{
  return m_Settings;
}

uint32_t ReSTIRDIRenderer::GetAccumulatedFrameCount() const
{
  return IsAccumulationResolveMode(m_Settings.common.resolveMode) ? m_AccumulatedFrames : 0;
}

uint32_t ReSTIRDIRenderer::GetPipelineBounceLimit() const
{
  return m_PipelineBounceLimit;
}

void ReSTIRDIRenderer::InvalidateHistory()
{
  // CPU-side flags are reset immediately; GPU buffers are cleared on the next command buffer.
  m_AccumulatedFrames   = 0;
  m_HistoryInvalidated  = true;
  m_HasAccumulationSignature = false;
  m_HasDenoiserSignature = false;
  m_NeedsHistoryClear   = true;
  m_FrameContext.InvalidateHistory();
  m_ParameterContext.reset();
  m_NrdDenoiser.InvalidateHistory();
}

nvvk::DescriptorPack& ReSTIRDIRenderer::GetDescriptorPack()
{
  return m_DescPack;
}

const nvvk::DescriptorPack& ReSTIRDIRenderer::GetDescriptorPack() const
{
  return m_DescPack;
}

void ReSTIRDIRenderer::Render(const RenderInput& input)
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

  // ReSTIR is a frame algorithm: prepare state, upload parameters, record passes.
  EnsureViewportResources(viewportSize);
  FrameState frameState = BeginReSTIRFrame(input, viewportSize);
  PrepareDenoiser(input, frameState);

  // The descriptor set index follows nvpro's frame cycle to avoid overwriting in-flight data.
  const uint32_t frameSetIndex = GetReSTIRDIFrameSetIndex(m_App->getFrameCycleIndex(), m_ParameterBuffers.size());
  UpdateParameterBuffer(frameSetIndex, BuildShaderParameters());
  UpdateFrameDescriptors(input);
  PrepareStorageImages(input, frameState.denoiserSignalsNeeded);
  ClearHistoryIfNeeded(input.cmd);

  const shaderio::ReSTIRDIPushConstant pushConstant = BuildPushConstant(input, frameState.denoiserSignalsNeeded);
  // Push constants are small per-dispatch values; the larger ReSTIR settings live in the parameter buffer.
  RecordReSTIRPasses(input, pushConstant);
  RunDenoiserIfNeeded(input, frameState);
  FinishFrame(frameState);
}

bool ReSTIRDIRenderer::CanRender(const RenderInput& input) const
{
  return IsReady() && input.cmd != VK_NULL_HANDLE && input.sceneResource != nullptr && input.sceneInfo != nullptr
         && input.topLevelAS != nullptr && input.topLevelAS->accel != VK_NULL_HANDLE && input.gBuffers != nullptr;
}

void ReSTIRDIRenderer::EnsureViewportResources(VkExtent2D viewportSize)
{
  // Viewport-sized resources must exist before descriptors point at them.
  m_FrameContext.EnsureViewport(viewportSize);
  m_Resources.EnsureForViewport(viewportSize);
  m_DenoiserResources.EnsureForViewport(viewportSize);
  EnsureParameterContext(viewportSize);
}

ReSTIRDIRenderer::FrameState ReSTIRDIRenderer::BeginReSTIRFrame(const RenderInput& input, VkExtent2D viewportSize)
{
  FrameState frameState{};
  // FrameState keeps all history decisions together so the frame finishes with the same assumptions.
  frameState.viewportSize            = viewportSize;
  frameState.denoiseEnabled          = IsDenoiseResolveMode(m_Settings.common.resolveMode);
  frameState.restirDebugActive       = m_Settings.common.debugView != shaderio::eReSTIRDebugViewDisabled;
  frameState.denoiserSignalsNeeded   = frameState.denoiseEnabled && !frameState.restirDebugActive;
  frameState.accumulationSignature   = MakeReSTIRDIAccumulationSignature(*input.sceneInfo, input.topLevelAS->address, viewportSize);
  frameState.denoiserSignature       = MakeReSTIRDIDenoiserHistorySignature(*input.sceneInfo, input.topLevelAS->address, viewportSize);

  // Signatures tell us when old temporal or accumulation history no longer matches the scene.
  const bool accumulationSignatureChanged =
      !m_HasAccumulationSignature
      || std::memcmp(&frameState.accumulationSignature, &m_LastAccumulationSignature, sizeof(AccumulationSignature)) != 0;
  const bool restirHistoryInvalidated =
      m_HistoryInvalidated || (IsAccumulationResolveMode(m_Settings.common.resolveMode) && accumulationSignatureChanged);
  const bool denoiserSignatureChanged =
      !m_HasDenoiserSignature || std::memcmp(&frameState.denoiserSignature, &m_LastDenoiserSignature, sizeof(DenoiserSignature)) != 0;
  frameState.denoiserHistoryInvalidated = m_HistoryInvalidated || (frameState.denoiseEnabled && denoiserSignatureChanged);
  if(restirHistoryInvalidated)
  {
    // Recreating the parameter context resets reservoir rotation to a known first-frame state.
    m_AccumulatedFrames = 0;
    m_FrameContext.InvalidateHistory();
    m_ParameterContext.reset();
    EnsureParameterContext(viewportSize);
    m_NeedsHistoryClear = true;
  }

  return frameState;
}

void ReSTIRDIRenderer::PrepareDenoiser(const RenderInput& input, const FrameState& frameState)
{
  if(frameState.denoiseEnabled)
  {
    // NRD needs guide buffers and matching camera history before the ReSTIR passes write signals.
    m_NrdDenoiser.PrepareFrame(NrdDenoiser::FrameInput{
                                   .sceneInfo          = input.sceneInfo,
                                   .viewportSize       = frameState.viewportSize,
                                   .historyInvalidated = frameState.denoiserHistoryInvalidated,
                                   .enableMaterialDemodulation = true,
                                   .settings           = &m_Settings.common.denoiserSettings,
                               },
                               m_DenoiserResources);
  }
}

shaderio::ReSTIRDIParameters ReSTIRDIRenderer::BuildShaderParameters()
{
  // The parameter context converts UI/settings state into the one GPU uniform block.
  m_ParameterContext->SetFrameIndex(m_FrameContext.GetFrameIndex());
  m_ParameterContext->SetResamplingMode(m_Settings.common.resamplingMode);
  m_ParameterContext->SetInitialSamplingParameters(m_Settings.initialSampling);
  m_ParameterContext->SetTemporalResamplingParameters(m_Settings.temporalResampling);
  m_ParameterContext->SetSpatialResamplingParameters(m_Settings.spatialResampling);
  m_ParameterContext->SetShadingParameters(m_Settings.shading);
  // The UI value cannot exceed the recursion depth supported by the current device/pipeline.
  m_Settings.secondaryPathMaxBounces = std::min(m_Settings.secondaryPathMaxBounces, m_PipelineBounceLimit);

  return shaderio::ReSTIRDIParameters{
      .runtimeParams         = m_ParameterContext->GetRuntimeParameters(),
      .reservoirBufferParams = m_ParameterContext->GetReservoirBufferParameters(),
      .bufferIndices         = m_ParameterContext->GetBufferIndices(),
      .initialSampling       = m_ParameterContext->GetInitialSamplingParameters(),
      .temporalResampling    = m_ParameterContext->GetTemporalResamplingParameters(),
      .spatialResampling     = m_ParameterContext->GetSpatialResamplingParameters(),
      .shading               = m_ParameterContext->GetShadingParameters(),
  };
}

void ReSTIRDIRenderer::PrepareStorageImages(const RenderInput& input, bool denoiserSignalsNeeded)
{
  // The ray tracing passes write directly into the accumulation and output images.
  TransitionReSTIRDIStorageImages(input.cmd, const_cast<nvvk::Image&>(m_Resources.GetAccumulationImage()),
                                 input.gBuffers->getColorImage(input.renderedImageIndex),
                                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  if(denoiserSignalsNeeded)
  {
    // Denoiser guide buffers are storage images written by the final ReSTIR shading pass.
    TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetMotionVectorsImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetNormalRoughnessImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetBaseColorMetalnessImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetViewZImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetDiffuseRadianceHitDistanceImage(),
                                   VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetSpecularRadianceHitDistanceImage(),
                                   VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
    TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetSpecularDemodulationFactorImage(),
                                   VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  }
}

void ReSTIRDIRenderer::ClearHistoryIfNeeded(VkCommandBuffer cmd)
{
  if(m_NeedsHistoryClear)
  {
    // Reset reservoirs and surface history after scene, camera, or viewport changes.
    ClearHistoryBuffers(cmd);
    m_NeedsHistoryClear = false;
  }
}

shaderio::ReSTIRDIPushConstant ReSTIRDIRenderer::BuildPushConstant(const RenderInput& input, bool denoiserSignalsNeeded) const
{
  uint32_t restirFlags = 0u;
  if(IsAccumulationResolveMode(m_Settings.common.resolveMode))
  {
    restirFlags |= shaderio::eReSTIRFlagAccumulate;
  }
  if(denoiserSignalsNeeded)
  {
    restirFlags |= shaderio::eReSTIRFlagWriteDenoiserSignals;
  }

  // Push constants carry per-dispatch data that changes more often than descriptors.
  return shaderio::ReSTIRDIPushConstant{
      .sceneInfoAddress       = (shaderio::GltfSceneInfo*)input.sceneResource->bSceneInfo.address,
      .accumulatedFrames      = IsAccumulationResolveMode(m_Settings.common.resolveMode) ? m_AccumulatedFrames : 0u,
      .flags                  = restirFlags,
      .secondaryPathMaxBounces = m_Settings.secondaryPathMaxBounces,
      .debugView              = static_cast<uint32_t>(m_Settings.common.debugView),
  };
}

void ReSTIRDIRenderer::RecordReSTIRPasses(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant)
{
  // The pass order mirrors the ReSTIR DI algorithm: candidate, temporal, spatial, shade.
  RunInitialSamplingPass(input, pushConstant);

  const bool enableTemporal = IsReSTIRDITemporalResamplingEnabled(m_Settings.common.resamplingMode);
  const bool enableSpatial  = IsReSTIRDISpatialResamplingEnabled(m_Settings.common.resamplingMode);

  VkPipelineStageFlags2 lastStage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
  if(enableTemporal)
  {
    // Initial sampling writes reservoirs/surfaces that temporal compute reads.
    nvvk::cmdMemoryBarrier(input.cmd, lastStage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    RunTemporalPass(input, pushConstant);
    lastStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  }

  if(enableSpatial)
  {
    // Spatial reuse consumes either the temporal result or the initial reservoir.
    nvvk::cmdMemoryBarrier(input.cmd, lastStage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    RunSpatialPass(input, pushConstant);
    lastStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  }

  // Final shading reads the chosen reservoir and writes the output/accumulation images.
  nvvk::cmdMemoryBarrier(input.cmd, lastStage, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  RunFinalShadingPass(input, pushConstant);
  // Leave the image writes visible to later post-processing or denoising work.
  nvvk::cmdMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void ReSTIRDIRenderer::RunDenoiserIfNeeded(const RenderInput& input, const FrameState& frameState)
{
  if(frameState.denoiseEnabled && !frameState.restirDebugActive && m_NrdDenoiser.IsReady())
  {
    m_NrdDenoiser.Denoise(input.cmd, m_DenoiserResources, m_Resources.GetAccumulationImage().descriptor.imageView,
                          input.gBuffers->getColorImageView(input.renderedImageIndex), m_Settings.common.denoiserDebugView,
                          frameState.viewportSize);
  }
  else if(frameState.denoiseEnabled && frameState.restirDebugActive)
  {
    // Debug views replace the beauty image, so NRD history should not continue through them.
    m_NrdDenoiser.InvalidateHistory();
  }
}

void ReSTIRDIRenderer::FinishFrame(const FrameState& frameState)
{
  // Store history signatures after all passes used the current frame state.
  m_LastAccumulationSignature = frameState.accumulationSignature;
  m_LastDenoiserSignature = frameState.denoiserSignature;
  m_HasAccumulationSignature = true;
  m_HasDenoiserSignature = true;
  m_HistoryInvalidated   = false;
  m_FrameContext.AdvanceFrame();
  m_AccumulatedFrames = IsAccumulationResolveMode(m_Settings.common.resolveMode) ? (m_AccumulatedFrames + 1u) : 0u;
}

void ReSTIRDIRenderer::QueryRayTracingProperties()
{
  VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m_RtProperties};
  vkGetPhysicalDeviceProperties2(m_Allocator->getPhysicalDevice(), &props);
  // Vulkan recursion depth counts the primary ray, so secondary path bounces get one less.
  const uint32_t maxBounceLimit     = (m_RtProperties.maxRayRecursionDepth > 0) ? (m_RtProperties.maxRayRecursionDepth - 1u) : 0u;
  m_PipelineBounceLimit             = std::min(kRequestedMaxBounces, maxBounceLimit);
  m_Settings.secondaryPathMaxBounces = std::min(m_Settings.secondaryPathMaxBounces, m_PipelineBounceLimit);
}

void ReSTIRDIRenderer::CreateDescriptorSetLayout()
{
  // One descriptor layout is shared by all ReSTIR passes so the pass sequence can reuse the same set.
  const VkShaderStageFlags allStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR
                                       | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR
                                       | VK_SHADER_STAGE_COMPUTE_BIT;

  nvvk::DescriptorBindings bindings;
  // Textures use bindless-style indexing from material records.
  bindings.addBinding({.binding = shaderio::ReSTIRDIBindingPoints::eReSTIRDITextures,
                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       .descriptorCount = m_MaxTextureDescriptors,
                       .stageFlags = allStages},
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                          | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDITlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, allStages);
  // Storage images are written by final shading and optionally consumed by NRD.
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDIOutputImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDIAccumulationImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDILightReservoirBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  // Surface buffers are ping-ponged for current/previous-frame temporal reuse.
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDICurrentSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDIPreviousSurfaceBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDINeighborOffsetBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDIParamsBuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDIDebugBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDIMotionVectorsImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDINormalRoughnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDIBaseColorMetalnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDIViewZImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDIDiffuseRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                      allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDISpecularRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                      allStages);
  bindings.addBinding(shaderio::ReSTIRDIBindingPoints::eReSTIRDISpecularDemodulationFactorImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                      allStages);

  m_DescPack.init(bindings, m_Allocator->getDevice(), m_App->getFrameCycleSize(), VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                  VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
}

void ReSTIRDIRenderer::CreatePipelineLayout()
{
  // Pipeline layout is the ABI between C++ descriptor sets/push constants and Slang bindings.
  const VkPushConstantRange pushConstantRange{
      .stageFlags = kReSTIRDIPushConstantStages,
      .offset     = 0,
      .size       = sizeof(shaderio::ReSTIRDIPushConstant),
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

void ReSTIRDIRenderer::CreateParameterBuffers()
{
  const uint32_t frameSetCount = std::max(1u, m_App->getFrameCycleSize());
  m_ParameterBuffers.resize(frameSetCount);

  for(nvvk::Buffer& parameterBuffer : m_ParameterBuffers)
  {
    // Mapped uniform buffers are updated once per frame set before recording the ReSTIR passes.
    NVVK_CHECK(m_Allocator->createBuffer(
        parameterBuffer, sizeof(shaderio::ReSTIRDIParameters), VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT));
    NVVK_DBG_NAME(parameterBuffer.buffer);
  }
}

void ReSTIRDIRenderer::CreateInitialSamplingPipeline()
{
  CreateReSTIRDIRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetInitialSamplingShaderCode(), 2u,
                               "ReSTIR DI Initial Sampling Pipeline", m_InitialSamplingPass);
}

void ReSTIRDIRenderer::CreateFinalShadingPipeline()
{
  CreateReSTIRDIRayTracingPass(m_Allocator, m_RtProperties, m_PipelineLayout, GetFinalShadingShaderCode(),
                               std::max(1u, m_PipelineBounceLimit + 1u), "ReSTIR DI Final Shading Pipeline",
                               m_FinalShadingPass);
}

void ReSTIRDIRenderer::CreateComputePipelines()
{
  m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)] =
      CreateReSTIRDIComputePipeline(m_Allocator, m_PipelineLayout, GetTemporalShaderCode(), "ReSTIR DI Temporal Pipeline");
  m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)] =
      CreateReSTIRDIComputePipeline(m_Allocator, m_PipelineLayout, GetSpatialShaderCode(), "ReSTIR DI Spatial Pipeline");
}

void ReSTIRDIRenderer::EnsureParameterContext(VkExtent2D viewportSize)
{
  if(m_ParameterContext != nullptr)
  {
    const ReSTIRDIStaticParameters& staticParameters = m_ParameterContext->GetStaticParameters();
    if(staticParameters.renderWidth == viewportSize.width && staticParameters.renderHeight == viewportSize.height
       && staticParameters.neighborOffsetCount == m_Resources.GetNeighborOffsetCount())
    {
      return;
    }
  }

  // The reservoir addressing math depends on resolution and neighbor table size.
  const ReSTIRDIStaticParameters staticParameters{
      .neighborOffsetCount      = m_Resources.GetNeighborOffsetCount(),
      .renderWidth              = viewportSize.width,
      .renderHeight             = viewportSize.height,
  };
  m_ParameterContext = std::make_unique<ReSTIRDIParameterContext>(staticParameters);
  m_NeedsHistoryClear = true;
}

void ReSTIRDIRenderer::UpdateFrameDescriptors(const RenderInput& input)
{
  const uint32_t frameSetIndex        = GetReSTIRDIFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  const uint32_t currentHistoryIndex  = m_FrameContext.GetCurrentHistoryIndex();
  const uint32_t previousHistoryIndex = m_FrameContext.GetPreviousHistoryIndex();

  // Descriptors describe the concrete images and buffers used by this frame.
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
  VkDescriptorImageInfo specularDemodulationFactorImageInfo = m_DenoiserResources.GetSpecularDemodulationFactorImage().descriptor;
  specularDemodulationFactorImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

  std::array<VkDescriptorBufferInfo, 6> bufferInfos{
      // Reservoir buffer contains all three logical reservoir arrays.
      VkDescriptorBufferInfo{m_Resources.GetLightReservoirBuffer().buffer, 0, VK_WHOLE_SIZE},
      // Surface buffers are bound as current/previous according to frame parity.
      VkDescriptorBufferInfo{m_Resources.GetSurfaceBuffer(currentHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetSurfaceBuffer(previousHistoryIndex).buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_Resources.GetNeighborOffsetBuffer().buffer, 0, VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{m_ParameterBuffers[frameSetIndex].buffer, 0, sizeof(shaderio::ReSTIRDIParameters)},
      VkDescriptorBufferInfo{m_Resources.GetDebugBuffer().buffer, 0, VK_WHOLE_SIZE},
  };

  VkAccelerationStructureKHR accel = input.topLevelAS->accel;
  // TLAS descriptors are attached through pNext rather than pBufferInfo/pImageInfo.
  VkWriteDescriptorSetAccelerationStructureKHR accelerationInfo{
      .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
      .accelerationStructureCount = 1,
      .pAccelerationStructures    = &accel,
  };

  std::array<VkWriteDescriptorSet, 16> writes{};
  uint32_t                            writeCount = 0;

  // TLAS and output images are written individually because their descriptor types differ.
  writes[writeCount]       = m_DescPack.makeWrite(shaderio::ReSTIRDIBindingPoints::eReSTIRDITlas, frameSetIndex);
  writes[writeCount].pNext = &accelerationInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.makeWrite(shaderio::ReSTIRDIBindingPoints::eReSTIRDIOutputImage, frameSetIndex);
  writes[writeCount].pImageInfo = &outputImageInfo;
  ++writeCount;

  writes[writeCount]            = m_DescPack.makeWrite(shaderio::ReSTIRDIBindingPoints::eReSTIRDIAccumulationImage, frameSetIndex);
  writes[writeCount].pImageInfo = &accumulationImageInfo;
  ++writeCount;

  const std::array<uint32_t, 4> bufferBindings{
      shaderio::ReSTIRDIBindingPoints::eReSTIRDILightReservoirBuffer,
      shaderio::ReSTIRDIBindingPoints::eReSTIRDICurrentSurfaceBuffer,
      shaderio::ReSTIRDIBindingPoints::eReSTIRDIPreviousSurfaceBuffer,
      shaderio::ReSTIRDIBindingPoints::eReSTIRDINeighborOffsetBuffer,
  };

  // The first four buffer infos line up with the first four buffer bindings above.
  for(size_t i = 0; i < bufferBindings.size(); ++i)
  {
    writes[writeCount]             = m_DescPack.makeWrite(bufferBindings[i], frameSetIndex);
    writes[writeCount].pBufferInfo = &bufferInfos[i];
    ++writeCount;
  }

  writes[writeCount]             = m_DescPack.makeWrite(shaderio::ReSTIRDIBindingPoints::eReSTIRDIParamsBuffer, frameSetIndex);
  writes[writeCount].pBufferInfo = &bufferInfos[4];
  ++writeCount;

  writes[writeCount]             = m_DescPack.makeWrite(shaderio::ReSTIRDIBindingPoints::eReSTIRDIDebugBuffer, frameSetIndex);
  writes[writeCount].pBufferInfo = &bufferInfos[5];
  ++writeCount;

  const std::array<uint32_t, 7> imageBindings{
      shaderio::ReSTIRDIBindingPoints::eReSTIRDIMotionVectorsImage,
      shaderio::ReSTIRDIBindingPoints::eReSTIRDINormalRoughnessImage,
      shaderio::ReSTIRDIBindingPoints::eReSTIRDIBaseColorMetalnessImage,
      shaderio::ReSTIRDIBindingPoints::eReSTIRDIViewZImage,
      shaderio::ReSTIRDIBindingPoints::eReSTIRDIDiffuseRadianceHitDistanceImage,
      shaderio::ReSTIRDIBindingPoints::eReSTIRDISpecularRadianceHitDistanceImage,
      shaderio::ReSTIRDIBindingPoints::eReSTIRDISpecularDemodulationFactorImage,
  };
  const std::array<VkDescriptorImageInfo*, 7> imageInfos{
      &motionVectorsImageInfo,
      &normalRoughnessImageInfo,
      &baseColorMetalnessImageInfo,
      &viewZImageInfo,
      &diffuseRadianceHitDistanceImageInfo,
      &specularRadianceHitDistanceImageInfo,
      &specularDemodulationFactorImageInfo,
  };
  // NRD guide images are always bound; final shading only writes them when the flag is enabled.
  for(size_t i = 0; i < imageBindings.size(); ++i)
  {
    writes[writeCount]            = m_DescPack.makeWrite(imageBindings[i], frameSetIndex);
    writes[writeCount].pImageInfo = imageInfos[i];
    ++writeCount;
  }

  vkUpdateDescriptorSets(m_Allocator->getDevice(), writeCount, writes.data(), 0, nullptr);
}

void ReSTIRDIRenderer::ClearHistoryBuffers(VkCommandBuffer cmd)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(cmd, "ReSTIR DI Clear History");
  // Zeroed reservoirs/surfaces make the first frame behave like there is no temporal history.
  vkCmdFillBuffer(cmd, m_Resources.GetLightReservoirBuffer().buffer, 0, m_Resources.GetLightReservoirBuffer().bufferSize, 0);
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
          .buffer        = m_Resources.GetLightReservoirBuffer().buffer,
          .offset        = 0,
          .size          = m_Resources.GetLightReservoirBuffer().bufferSize,
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

  // Transfer writes from vkCmdFillBuffer must be visible before ray tracing/compute reads.
  const VkDependencyInfo dependencyInfo{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = uint32_t(std::size(barriers)),
      .pBufferMemoryBarriers    = barriers,
  };
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

void ReSTIRDIRenderer::RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR DI Initial Sampling");
  const uint32_t frameSetIndex = GetReSTIRDIFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  TraceReSTIRDIRayTracingPass(input.cmd, m_InitialSamplingPass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                            kReSTIRDIPushConstantStages, pushConstant, input.gBuffers->getSize());
}

void ReSTIRDIRenderer::RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR DI Temporal Resampling");
  const uint32_t frameSetIndex = GetReSTIRDIFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  DispatchReSTIRDIComputePass(input.cmd, m_ComputePipelines[static_cast<size_t>(ComputePass::eTemporal)], m_PipelineLayout,
                            *m_DescPack.getSetPtr(frameSetIndex), kReSTIRDIPushConstantStages, pushConstant, input.gBuffers->getSize(),
                            kComputeGroupSize);
}

void ReSTIRDIRenderer::RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR DI Spatial Resampling");
  const uint32_t frameSetIndex = GetReSTIRDIFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  DispatchReSTIRDIComputePass(input.cmd, m_ComputePipelines[static_cast<size_t>(ComputePass::eSpatial)], m_PipelineLayout,
                            *m_DescPack.getSetPtr(frameSetIndex), kReSTIRDIPushConstantStages, pushConstant, input.gBuffers->getSize(),
                            kComputeGroupSize);
}

void ReSTIRDIRenderer::RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRDIPushConstant& pushConstant)
{
  nvvk::DebugUtil::ScopedCmdLabel scopedCmdLabel(input.cmd, "ReSTIR DI Final Shading");
  const uint32_t frameSetIndex = GetReSTIRDIFrameSetIndex(m_App->getFrameCycleIndex(), m_DescPack.getSets().size());
  TraceReSTIRDIRayTracingPass(input.cmd, m_FinalShadingPass, m_PipelineLayout, *m_DescPack.getSetPtr(frameSetIndex),
                            kReSTIRDIPushConstantStages, pushConstant, input.gBuffers->getSize());
}

void ReSTIRDIRenderer::UpdateParameterBuffer(uint32_t frameSetIndex, const shaderio::ReSTIRDIParameters& parameters)
{
  if(frameSetIndex >= m_ParameterBuffers.size())
  {
    return;
  }

  // Host writes to the mapped uniform buffer are flushed before command recording uses it.
  nvvk::Buffer& parameterBuffer = m_ParameterBuffers[frameSetIndex];
  std::memcpy(parameterBuffer.mapping, &parameters, sizeof(parameters));
  NVVK_CHECK(m_Allocator->flushBuffer(parameterBuffer, 0, sizeof(parameters)));
}

}  // namespace nvsamples
