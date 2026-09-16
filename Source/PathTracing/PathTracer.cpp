#include "PathTracer.h"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include "Generated/Shaders/PathTracer.hlsl.library.h"
#include "Framework/Vulkan/Barriers.h"
#include "Framework/Vulkan/Pipelines.h"
#include "Sampling/SpatiotemporalBlueNoise.h"

namespace rtpt
{

namespace
{

// Longest path the settings allow. Ray generation traces every bounce from a loop, so this caps cost rather than reflecting a device limit.
constexpr uint32_t kMaxBounces = 8;

// Ray generation traces every camera, bounce, and shadow ray itself, and no closest-hit or miss shader traces, so the pipeline needs a single level of recursion.
constexpr uint32_t kPipelineRecursionDepth = 1;

// Full float RGBA for the HDR path radiance the accumulation image holds before tonemapping or denoising.
constexpr VkFormat kAccumulationFormat  = VK_FORMAT_R32G32B32A32_SFLOAT;

}  // namespace

PathTracer::PathTracer(const CreateInfo& createInfo)
    : m_Device(createInfo.device)
    , m_Resources(createInfo.resources)
    , m_Diagnostics(createInfo.diagnostics)
    , m_BlueNoise(createInfo.blueNoise)
    , m_FrameSlotCount(createInfo.frameSlotCount)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
    , m_History(ResolveHistory::CreateInfo { .device = createInfo.device, .resources = createInfo.resources, .diagnostics = createInfo.diagnostics, .frameSlotCount = createInfo.frameSlotCount })
{
}

void PathTracer::Initialize()
{
  // A missing dependency leaves the renderer unready rather than half-initialized; IsReady reports it.
  if(m_Device == nullptr || m_Resources == nullptr || m_BlueNoise == nullptr || m_FrameSlotCount == 0 || m_MaxTextureDescriptors == 0)
  {
    return;
  }

  // Permanent Vulkan objects
  // Created here in dependency order: the device's ray tracing limits are checked first, the descriptor layout feeds the pipeline layout, and the SBT is built from the pipeline.
  // Viewport-sized images are created when frames arrive.

  QueryRayTracingProperties();
  CreateDescriptorSetLayout();
  CreatePipelineLayout();
  CreateRayTracingPipeline();
  CreateShaderBindingTable();

  m_History.Initialize();

  InvalidateHistory();
}

void PathTracer::Destroy()
{
  // Without an allocator and a device nothing below was ever created, and every destroy call below needs the device handle.
  if(m_Resources == nullptr || m_Device == nullptr)
  {
    return;
  }

  VkDevice device = m_Device->Handle();

  // GPU resources
  // Destroy GPU resources before the descriptor/pipeline state that references them.

  DestroyAccumulationImage();
  m_History.Destroy();

  // Pipeline state
  // The SBT is built from the pipeline, and the pipeline from the layout, so they go in that order.

  m_Sbt.Destroy();

  vkDestroyPipeline(device, m_Pipeline, nullptr);
  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);

  m_Pipeline       = VK_NULL_HANDLE;
  m_PipelineLayout = VK_NULL_HANDLE;

  m_DescPack.Destroy();

  // History reset
  // Leaves the object in its freshly constructed state, so a later Initialize starts clean.

  m_RngFrameNumber = 0;
}

bool PathTracer::IsReady() const
{
  return m_Pipeline != VK_NULL_HANDLE && m_PipelineLayout != VK_NULL_HANDLE && m_Sbt.Storage();
}

PathTracer::Settings& PathTracer::GetSettings()
{
  return m_Settings;
}

const PathTracer::Settings& PathTracer::GetSettings() const
{
  return m_Settings;
}

uint32_t PathTracer::GetAccumulatedFrameCount() const
{
  // Outside accumulate mode the internal counter is not a meaningful sample count.
  return IsAccumulationResolveMode(m_Settings.resolveMode) ? m_History.GetAccumulatedFrameCount() : 0;
}

uint32_t PathTracer::GetBounceLimit() const
{
  return kMaxBounces;
}

void PathTracer::InvalidateHistory()
{
  m_History.InvalidateHistory();
}

rtpt::DescriptorPack& PathTracer::GetDescriptorPack()
{
  return m_DescPack;
}

const rtpt::DescriptorPack& PathTracer::GetDescriptorPack() const
{
  return m_DescPack;
}

void PathTracer::Render(const RenderInput& input)
{
  if(!CanRender(input))
  {
    return;
  }

  const VkExtent2D viewportSize = input.output.extent;

  // A zero extent has nothing to trace into. The HDR target never has one, because Application skips frames while minimized and ViewportTargets refuses empty sizes, so this is only a defensive guard.
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  // Frame preparation
  // Path tracing is a frame algorithm: prepare history, trace, optionally denoise.
  // Resources come first because the descriptors written below point at them. The history decisions, including NRD's frame setup, are taken once in BeginFrame so the dispatch, the denoiser, and FinishFrame all agree on history validity.

  EnsureViewportResources(viewportSize);

  const ResolveHistory::FrameState frameState = m_History.BeginFrame(ResolveHistory::FrameInput { .sceneInfo = input.sceneInfo, .topLevelAsAddress = input.topLevelAS->address, .viewportSize = viewportSize, .resolveMode = m_Settings.resolveMode, .denoiserSignalsAvailable = true, .denoiserSettings = &m_Settings.denoiserSettings, .frameSlot = input.frameSlot, .frameTimeMilliseconds = input.frameTimeMilliseconds });

  UpdateFrameDescriptors(input);
  PrepareStorageImages(input, frameState);

  // Trace and resolve
  // Push constants carry the small per-dispatch values; descriptors carry images, TLAS, and textures.
  // NRD reads the accumulation image as its noisy beauty input and composes into the output target.

  const shaderio::PathTracePushConstant pushConstant = BuildPushConstant(input, frameState);

  RecordPathTracePass(input, pushConstant);

  // Only a frame that denoises runs NRD, so only that frame opens the denoiser scope.
  {
    const GpuProfiler::Zone denoiserZone(frameState.denoiseEnabled ? input.profiler : nullptr, input.cmd, FrameSlot { input.frameSlot }, "Path tracer/Denoiser");

    m_History.Denoise(input.cmd, frameState, m_AccumulationImage.descriptor.imageView, input.output.view, m_Settings.denoiserDebugView);
  }

  m_History.FinishFrame(frameState);
}

bool PathTracer::CanRender(const RenderInput& input) const
{
  return IsReady() && input.cmd != VK_NULL_HANDLE && input.sceneResource != nullptr && input.sceneInfo != nullptr && input.topLevelAS != nullptr && input.topLevelAS->accel != VK_NULL_HANDLE && input.output;
}

void PathTracer::EnsureViewportResources(VkExtent2D viewportSize)
{
  // Viewport-sized resources must exist before descriptors point at them.
  CreateOrResizeAccumulationImage(viewportSize);
  m_History.EnsureViewportResources(viewportSize);
}

void PathTracer::PrepareStorageImages(const RenderInput& input, const ResolveHistory::FrameState& frameState)
{
  // Renderer-owned images
  // The ray generation shader always writes beauty/accumulation. NRD guide images are only written for denoised frames so raw timing is not charged for NRD setup.
  // This renderer only transitions images out of UNDEFINED after creation; images already in GENERAL get no barrier.

  TransitionStorageImageForWrite(input.cmd, m_AccumulationImage, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, StorageImageWriteOrdering::eFirstTransitionOnly);

  if(frameState.denoiseEnabled)
  {
    m_History.TransitionGuideImagesForWrite(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, StorageImageWriteOrdering::eFirstTransitionOnly);
  }

  // Output image
  // The swapchain/G-buffer color target is external to PathTracer but written as a storage image here.
  // It is entered from UNDEFINED every frame, so its previous contents are discarded and the shader only needs write access.

  const VkImageMemoryBarrier2 outputBarrier {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask     = VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask    = VK_ACCESS_2_NONE,
      .dstStageMask     = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
      .dstAccessMask    = VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout        = VK_IMAGE_LAYOUT_GENERAL,
      .image            = input.output.image,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
  };

  const VkDependencyInfo outputDependency {
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &outputBarrier };

  vkCmdPipelineBarrier2(input.cmd, &outputDependency);
}

shaderio::PathTracePushConstant PathTracer::BuildPushConstant(const RenderInput& input, const ResolveHistory::FrameState& frameState)
{
  // Settings may hold any value; the shader loop only ever sees the renderer's fixed maximum.
  const uint32_t maxBounces = std::min(m_Settings.maxBounces, kMaxBounces);

  // Shader flags

  uint32_t pathTraceFlags = 0;

  if(frameState.accumulateEnabled)
  {
    pathTraceFlags |= shaderio::ePathTraceFlagAccumulate;
  }

  if(frameState.denoiseEnabled)
  {
    pathTraceFlags |= shaderio::ePathTraceFlagWriteDenoiserSignals;
  }

  // Push constants
  // Push constants carry per-dispatch data that changes more often than descriptors.
  // The RNG frame number advances on every call, independently of accumulation, so samples stay decorrelated after a history reset.

  return shaderio::PathTracePushConstant {
      .sceneInfoAddress        = (shaderio::GltfSceneInfo*)input.sceneResource->bSceneInfo.address,
      .rngFrameNumber          = m_RngFrameNumber++,
      .accumulatedFrames       = frameState.accumulateEnabled ? m_History.GetAccumulatedFrameCount() : 0,
      .maxBounces              = maxBounces,
      .flags                   = pathTraceFlags,
      .reblurHitDistanceParams = { m_Settings.denoiserSettings.hitDistanceA, m_Settings.denoiserSettings.hitDistanceB, m_Settings.denoiserSettings.hitDistanceC },
      .denoiserRadianceClamp   = ComputeDenoiserRadianceClamp(m_Settings.denoiserSettings, input.denoiserGreyLuminance),
  };
}

void PathTracer::RecordPathTracePass(const RenderInput& input, const shaderio::PathTracePushConstant& pushConstant)
{
  // The scope spans the whole pass, including the barrier that publishes its writes.
  const GpuProfiler::Zone traceZone(input.profiler, input.cmd, FrameSlot { input.frameSlot }, "Path tracer/Trace");

  // Bind state
  // The path tracer is a single ray tracing dispatch. The frame slot is clamped so an out-of-range slot still binds a valid set.

  const VkExtent2D viewportSize = input.output.extent;

  vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_Pipeline);

  const uint32_t frameSetIndex = std::min(input.frameSlot, uint32_t(m_DescPack.Sets().size() - 1));

  vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1, m_DescPack.SetPtr(frameSetIndex), 0, nullptr);

  // Stage flags must match the push constant range declared in CreatePipelineLayout.
  vkCmdPushConstants(input.cmd, m_PipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR, 0, sizeof(shaderio::PathTracePushConstant), &pushConstant);

  // Trace
  // One ray generation invocation per output pixel.

  const rtpt::ShaderBindingTableRegions& regions = m_Sbt.Regions();

  vkCmdTraceRaysKHR(input.cmd, &regions.raygen, &regions.miss, &regions.hit, &regions.callable, viewportSize.width, viewportSize.height, 1);

  // Make path-traced image and guide-buffer writes visible to later compute denoising/post work.
  rtpt::CmdMemoryBarrier(input.cmd, { .stages = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT }, { .stages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT });
}

void PathTracer::QueryRayTracingProperties()
{
  m_RtProperties = m_Device->Support().rayTracingProperties;

  // Path length no longer depends on recursion, but the pipeline still needs its one level. Vulkan guarantees that much on any device with ray tracing pipelines, so failing here means the reported properties are unusable.
  if(m_RtProperties.maxRayRecursionDepth < kPipelineRecursionDepth)
  {
    rtpt::CheckVk(VK_ERROR_FEATURE_NOT_PRESENT, "PathTracer::QueryRayTracingProperties(maxRayRecursionDepth)");
  }
}

void PathTracer::CreateDescriptorSetLayout()
{
  // One descriptor layout is shared by the path tracing shader and Application's texture updates.
  const VkShaderStageFlags rayTracingStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

  rtpt::DescriptorBindings bindings;

  // Scene bindings
  // Textures use bindless-style indexing from material records.
  // The arrays are sized to maxTextureDescriptors and flagged partially bound and update-after-bind, so not every element needs a valid texture.

  constexpr VkDescriptorBindingFlags textureFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

  bindings.Add(shaderio::BindingPoints::eTextures, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_MaxTextureDescriptors, rayTracingStages, textureFlags);
  bindings.Add(shaderio::BindingPoints::eHlslTextures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, m_MaxTextureDescriptors, rayTracingStages, textureFlags);
  bindings.Add(shaderio::BindingPoints::eHlslTextureSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, m_MaxTextureDescriptors, rayTracingStages, textureFlags);
  bindings.Add(shaderio::BindingPoints::eTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, rayTracingStages);

  // Ray generation images
  // Output/accumulation/NRD guide images are all written by ray generation, and the blue noise texture is only sampled there.

  bindings.Add(shaderio::BindingPoints::eOutputImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.Add(shaderio::BindingPoints::eAccumulationImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.Add(shaderio::BindingPoints::eMotionVectorsImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.Add(shaderio::BindingPoints::eNormalRoughnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.Add(shaderio::BindingPoints::eBaseColorMetalnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.Add(shaderio::BindingPoints::eViewZImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.Add(shaderio::BindingPoints::eDiffuseRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.Add(shaderio::BindingPoints::eSpecularRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.Add(shaderio::BindingPoints::eSpecularDemodulationFactorImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.Add(shaderio::BindingPoints::eBlueNoiseTexture, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, rayTracingStages);

  // Descriptor pack
  // One set per frame slot. The pool flags must match the update-after-bind texture bindings above.

  rtpt::CheckVk(m_DescPack.Initialize(m_Device->Handle(), bindings, m_FrameSlotCount, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT), "DescriptorPack::Initialize(path tracer)");
}

void PathTracer::CreatePipelineLayout()
{
  // Pipeline layout is the ABI between C++ descriptor sets/push constants and HLSL bindings.
  const VkPushConstantRange pushConstantRange {
      .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
      .offset     = 0,
      .size       = sizeof(shaderio::PathTracePushConstant),
  };

  rtpt::CheckVk(rtpt::CreatePipelineLayout(m_Device->Handle(), m_PipelineLayout, std::span(m_DescPack.LayoutPtr(), 1), std::span(&pushConstantRange, 1)), "CreatePipelineLayout(path tracer)");
}

void PathTracer::CreateRayTracingPipeline()
{
  // Shader stages
  // The library exports a primary miss and hit pair for path rays, and a shadow miss and any-hit pair for visibility rays.

  enum StageIndices
  {
    eRaygen,
    eMiss,
    eShadowMiss,
    eAnyHit,
    eShadowAnyHit,
    eClosestHit,
    eStageCount,
  };

  constexpr std::array<rtpt::RayTracingShaderStage, eStageCount> stages { {
      { VK_SHADER_STAGE_RAYGEN_BIT_KHR, "rgenMain" },
      { VK_SHADER_STAGE_MISS_BIT_KHR, "rmissMain" },
      { VK_SHADER_STAGE_MISS_BIT_KHR, "shadowMissMain" },
      { VK_SHADER_STAGE_ANY_HIT_BIT_KHR, "rahitMain" },
      { VK_SHADER_STAGE_ANY_HIT_BIT_KHR, "shadowAnyHitMain" },
      { VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "rchitMain" },
  } };

  // Shader groups
  // One template is edited and appended per group. Group order is the SBT order in CreateShaderBindingTable: raygen 0, miss 1-2, hit 3-4.

  VkRayTracingShaderGroupCreateInfoKHR group { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };

  group.anyHitShader       = VK_SHADER_UNUSED_KHR;
  group.closestHitShader   = VK_SHADER_UNUSED_KHR;
  group.generalShader      = VK_SHADER_UNUSED_KHR;
  group.intersectionShader = VK_SHADER_UNUSED_KHR;

  std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups;
  shaderGroups.reserve(5);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eRaygen;
  shaderGroups.push_back(group);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eMiss;
  shaderGroups.push_back(group);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eShadowMiss;
  shaderGroups.push_back(group);

  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.anyHitShader     = eAnyHit;
  group.closestHitShader = eClosestHit;
  shaderGroups.push_back(group);

  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.anyHitShader     = eShadowAnyHit;
  group.closestHitShader = VK_SHADER_UNUSED_KHR;
  shaderGroups.push_back(group);

  // Pipeline
  // The recursion depth is fixed; see kPipelineRecursionDepth.

  rtpt::CheckVk(rtpt::CreateRayTracingPipeline(m_Device->Handle(), m_PipelineLayout, std::span(PathTracer_hlsl), stages, shaderGroups, kPipelineRecursionDepth, m_Pipeline), "CreateRayTracingPipeline(path tracer)");

  if(m_Diagnostics != nullptr)
  {
    m_Diagnostics->SetObjectName(m_Device->Handle(), VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(m_Pipeline), "Path Tracing Pipeline");
  }
}

void PathTracer::CreateShaderBindingTable()
{
  // Group indices must match the group order in CreateRayTracingPipeline.
  constexpr std::array<uint32_t, 1> raygen { 0 };
  constexpr std::array<uint32_t, 2> miss { 1, 2 };
  constexpr std::array<uint32_t, 2> hit { 3, 4 };

  rtpt::CheckVk(m_Sbt.Initialize(*m_Resources, m_Pipeline, 5, m_RtProperties, { .raygen = raygen, .miss = miss, .hit = hit, .callable = {} }), "ShaderBindingTable::Initialize(path tracer)");
}

void PathTracer::UpdateFrameDescriptors(const RenderInput& input)
{
  const uint32_t frameSetIndex = std::min(input.frameSlot, uint32_t(m_DescPack.Sets().size() - 1));

  // TLAS
  // TLAS descriptors attach through pNext rather than pBufferInfo/pImageInfo.

  const VkAccelerationStructureKHR tlas = input.topLevelAS->accel;

  const VkWriteDescriptorSetAccelerationStructureKHR accelerationInfo {
      .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
      .accelerationStructureCount = 1,
      .pAccelerationStructures    = &tlas,
  };

  // Images
  // The storage images are all bound in GENERAL, the layout PrepareStorageImages leaves them in.
  // The loop stops one short of the end: the blue noise texture is last, is only sampled, and keeps the layout its own descriptor reports.

  std::array<VkDescriptorImageInfo, 10> images {
      input.output.Descriptor(VK_IMAGE_LAYOUT_GENERAL),
      m_AccumulationImage.descriptor,
      m_History.GetDenoiserResources().GetMotionVectorsImage().descriptor,
      m_History.GetDenoiserResources().GetNormalRoughnessImage().descriptor,
      m_History.GetDenoiserResources().GetBaseColorMetalnessImage().descriptor,
      m_History.GetDenoiserResources().GetViewZImage().descriptor,
      m_History.GetDenoiserResources().GetDiffuseRadianceHitDistanceImage().descriptor,
      m_History.GetDenoiserResources().GetSpecularRadianceHitDistanceImage().descriptor,
      m_History.GetDenoiserResources().GetSpecularDemodulationFactorImage().descriptor,
      m_BlueNoise->Descriptor(),
  };

  for(size_t index = 0; index + 1 < images.size(); ++index)
  {
    images[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }

  // Parallel to images above: bindings[i] receives images[i].
  constexpr std::array<uint32_t, 10> bindings {
      shaderio::BindingPoints::eOutputImage,
      shaderio::BindingPoints::eAccumulationImage,
      shaderio::BindingPoints::eMotionVectorsImage,
      shaderio::BindingPoints::eNormalRoughnessImage,
      shaderio::BindingPoints::eBaseColorMetalnessImage,
      shaderio::BindingPoints::eViewZImage,
      shaderio::BindingPoints::eDiffuseRadianceHitDistanceImage,
      shaderio::BindingPoints::eSpecularRadianceHitDistanceImage,
      shaderio::BindingPoints::eSpecularDemodulationFactorImage,
      shaderio::BindingPoints::eBlueNoiseTexture,
  };

  // Writes
  // Slot 0 is the TLAS; the image writes follow it. Every binding is rewritten each frame, so recreated viewport-sized images are always picked up.

  std::array<VkWriteDescriptorSet, 11> writes {};

  writes[0] = m_DescPack.MakeWrite(shaderio::BindingPoints::eTlas, frameSetIndex);
  writes[0].pNext = &accelerationInfo;

  for(size_t index = 0; index < images.size(); ++index)
  {
    writes[index + 1] = m_DescPack.MakeWrite(bindings[index], frameSetIndex);
    writes[index + 1].pImageInfo = &images[index];
  }

  vkUpdateDescriptorSets(m_Device->Handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void PathTracer::CreateOrResizeAccumulationImage(VkExtent2D size)
{
  // Existing accumulation target already matches the viewport.
  if(m_AccumulationImage.image != VK_NULL_HANDLE && m_AccumulationImage.extent.width == size.width && m_AccumulationImage.extent.height == size.height)
  {
    return;
  }

  // Image description
  // R32G32B32A32 keeps HDR path radiance before tonemapping or denoising.
  // STORAGE for the ray generation writes, SAMPLED so NRD can read it as its beauty input.

  VkImageCreateInfo imageInfo {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = kAccumulationFormat,
      .extent        = { .width = size.width, .height = size.height, .depth = 1 },
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

  VkImageViewCreateInfo viewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = imageInfo.format,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 } };

  // Allocation
  // The recorded layout starts UNDEFINED so the first TransitionStorageImageForWrite moves it to GENERAL.

  rtpt::Image nextImage;

  rtpt::CheckVk(m_Resources->CreateImage(nextImage, imageInfo, &viewInfo), "ResourceAllocator::CreateImage(path-tracing accumulation)");

  nextImage.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if(m_Diagnostics != nullptr)
  {
    m_Diagnostics->SetObjectName(m_Device->Handle(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(nextImage.image), "PathTracingAccumulationImage");
  }

  // Swap in
  // A new image holds no history, so accumulation and NRD history restart with it.

  m_AccumulationImage = std::move(nextImage);

  InvalidateHistory();
}

void PathTracer::DestroyAccumulationImage()
{
  m_AccumulationImage.Reset();
}

}  // namespace rtpt
