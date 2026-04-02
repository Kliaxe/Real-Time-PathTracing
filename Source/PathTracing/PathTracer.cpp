#include "PathTracer.h"

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

#include "_autogen/PathTracer.slang.h"

namespace nvsamples
{

namespace
{
constexpr uint32_t kRequestedMaxBounces = 8;
constexpr VkFormat kAccumulationFormat  = VK_FORMAT_R32G32B32A32_SFLOAT;

VkShaderModuleCreateInfo GetPathTracingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(PathTracer_slang));
}

bool IsFinalRadianceAccumulationEnabled(const PathTracer::Settings& settings)
{
  return IsAccumulationResolveMode(settings.resolveMode);
}

}  // namespace

PathTracer::PathTracer(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
    , m_DenoiserResources(PathTraceDenoiserResources::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
    , m_NrdDenoiser(PathTraceNrdDenoiser::CreateInfo{.app = createInfo.app, .allocator = createInfo.allocator})
{
}

void PathTracer::Initialize()
{
  if(m_App == nullptr || m_Allocator == nullptr || m_MaxTextureDescriptors == 0)
  {
    return;
  }

  QueryRayTracingProperties();
  CreateDescriptorSetLayout();
  CreatePipelineLayout();
  CreateRayTracingPipeline();
  CreateShaderBindingTable();
  m_NrdDenoiser.Initialize();
  InvalidateAccumulation();
}

void PathTracer::Destroy()
{
  if(m_Allocator == nullptr)
  {
    return;
  }

  VkDevice device = m_Allocator->getDevice();

  DestroyAccumulationImage();
  m_DenoiserResources.Destroy();
  m_NrdDenoiser.Destroy();

  m_Allocator->destroyBuffer(m_SbtBuffer);
  m_SbtBuffer = {};

  m_SbtGenerator.deinit();
  m_SbtRegions = {};

  vkDestroyPipeline(device, m_Pipeline, nullptr);
  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
  m_Pipeline       = VK_NULL_HANDLE;
  m_PipelineLayout = VK_NULL_HANDLE;

  m_DescPack.deinit();

  m_RngFrameNumber           = 0;
  m_MaxBounceLimit           = 0;
  m_PipelineBounceLimit      = 0;
  m_AccumulatedFrames        = 0;
  m_AccumulationInvalidated  = true;
  m_HasAccumulationSignature = false;
  m_HasDenoiserSignature     = false;
}

bool PathTracer::IsReady() const
{
  return m_Pipeline != VK_NULL_HANDLE && m_PipelineLayout != VK_NULL_HANDLE && m_SbtBuffer.buffer != VK_NULL_HANDLE;
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
  return IsFinalRadianceAccumulationEnabled(m_Settings) ? m_AccumulatedFrames : 0;
}

uint32_t PathTracer::GetPipelineBounceLimit() const
{
  return m_PipelineBounceLimit;
}

void PathTracer::InvalidateAccumulation()
{
  m_AccumulatedFrames        = 0;
  m_AccumulationInvalidated  = true;
  m_HasAccumulationSignature = false;
  m_HasDenoiserSignature     = false;
  m_NrdDenoiser.InvalidateHistory();
}

nvvk::DescriptorPack& PathTracer::GetDescriptorPack()
{
  return m_DescPack;
}

const nvvk::DescriptorPack& PathTracer::GetDescriptorPack() const
{
  return m_DescPack;
}

void PathTracer::Render(const RenderInput& input)
{
  if(!IsReady() || input.cmd == VK_NULL_HANDLE || input.sceneResource == nullptr || input.sceneInfo == nullptr || input.topLevelAS == nullptr
     || input.topLevelAS->accel == VK_NULL_HANDLE || input.gBuffers == nullptr)
  {
    return;
  }

  const VkExtent2D size = input.gBuffers->getSize();
  if(size.width == 0 || size.height == 0)
  {
    return;
  }

  CreateOrResizeAccumulationImage(size);
  m_DenoiserResources.EnsureForViewport(size);

  const AccumulationSignature currentSignature = MakeAccumulationSignature(input, size);
  const bool accumulationHistoryInvalidated = m_AccumulationInvalidated || !m_HasAccumulationSignature
                                  || std::memcmp(&currentSignature, &m_LastAccumulationSignature, sizeof(AccumulationSignature)) != 0;
  if(accumulationHistoryInvalidated)
  {
    m_AccumulatedFrames = 0;
  }

  const DenoiserSignature currentDenoiserSignature = MakeDenoiserSignature(input, size);
  const bool denoiserHistoryInvalidated = m_AccumulationInvalidated || !m_HasDenoiserSignature
                                          || std::memcmp(&currentDenoiserSignature, &m_LastDenoiserSignature, sizeof(DenoiserSignature)) != 0;

  m_NrdDenoiser.PrepareFrame(PathTraceNrdDenoiser::FrameInput{
                                 .sceneInfo          = input.sceneInfo,
                                 .viewportSize       = size,
                                 .historyInvalidated = denoiserHistoryInvalidated,
                                 .enableMaterialDemodulation = true,
                                 .settings           = &m_Settings.denoiserSettings,
                             },
                             m_DenoiserResources);

  UpdateFrameDescriptors(input);

  TransitionStorageImageForWrite(input.cmd, m_AccumulationImage, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetMotionVectorsImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetNormalRoughnessImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetBaseColorMetalnessImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetViewZImage(), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetDiffuseRadianceHitDistanceImage(),
                                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  TransitionStorageImageForWrite(input.cmd, m_DenoiserResources.GetSpecularRadianceHitDistanceImage(),
                                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);

  const VkImageMemoryBarrier2 outputBarrier{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
      .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .image         = input.gBuffers->getColorImage(input.renderedImageIndex),
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
  };
  const VkDependencyInfo outputDependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &outputBarrier};
  vkCmdPipelineBarrier2(input.cmd, &outputDependency);

  vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_Pipeline);
  const uint32_t frameSetIndex = std::min(m_App->getFrameCycleIndex(), uint32_t(m_DescPack.getSets().size() - 1));
  vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1, m_DescPack.getSetPtr(frameSetIndex), 0,
                          nullptr);

  const uint32_t clampedMaxBounces        = std::min(m_Settings.maxBounces, m_MaxBounceLimit);
  const uint32_t pipelineSafeMaxBounces   = std::min(clampedMaxBounces, m_PipelineBounceLimit);
  const bool     finalAccumulationEnabled = IsFinalRadianceAccumulationEnabled(m_Settings);
  uint32_t       pathTraceFlags           = 0;
  if(finalAccumulationEnabled)
  {
    pathTraceFlags |= shaderio::ePathTraceFlagAccumulate;
  }

  const shaderio::PathTracePushConstant pushConstant{
      .sceneInfoAddress  = (shaderio::GltfSceneInfo*)input.sceneResource->bSceneInfo.address,
      .rngFrameNumber    = m_RngFrameNumber++,
      .accumulatedFrames = finalAccumulationEnabled ? m_AccumulatedFrames : 0,
      .maxBounces        = pipelineSafeMaxBounces,
      .flags             = pathTraceFlags,
  };
  vkCmdPushConstants(input.cmd, m_PipelineLayout,
                     VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                         | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                     0, sizeof(shaderio::PathTracePushConstant), &pushConstant);

  vkCmdTraceRaysKHR(input.cmd, &m_SbtRegions.raygen, &m_SbtRegions.miss, &m_SbtRegions.hit, &m_SbtRegions.callable, size.width,
                    size.height, 1);

  nvvk::cmdMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  if(IsDenoiseResolveMode(m_Settings.resolveMode))
  {
    m_NrdDenoiser.Denoise(input.cmd, m_DenoiserResources, m_AccumulationImage.descriptor.imageView,
                          input.gBuffers->getColorImageView(input.renderedImageIndex), m_Settings.denoiserDebugView, size);
  }

  m_LastAccumulationSignature = currentSignature;
  m_LastDenoiserSignature     = currentDenoiserSignature;
  m_HasAccumulationSignature  = true;
  m_HasDenoiserSignature      = true;
  m_AccumulationInvalidated   = false;
  m_AccumulatedFrames         = finalAccumulationEnabled ? (m_AccumulatedFrames + 1) : 0;
}

void PathTracer::QueryRayTracingProperties()
{
  VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m_RtProperties};
  vkGetPhysicalDeviceProperties2(m_Allocator->getPhysicalDevice(), &props);

  m_MaxBounceLimit      = (m_RtProperties.maxRayRecursionDepth > 0) ? (m_RtProperties.maxRayRecursionDepth - 1) : 0;
  m_PipelineBounceLimit = std::min(kRequestedMaxBounces, m_MaxBounceLimit);
  m_Settings.maxBounces = m_PipelineBounceLimit;
}

void PathTracer::CreateDescriptorSetLayout()
{
  const VkShaderStageFlags rayTracingStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR
                                              | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

  nvvk::DescriptorBindings bindings;
  bindings.addBinding({.binding = shaderio::BindingPoints::eTextures, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       .descriptorCount = m_MaxTextureDescriptors, .stageFlags = rayTracingStages},
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                          | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
  bindings.addBinding(shaderio::BindingPoints::eTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, rayTracingStages);
  bindings.addBinding(shaderio::BindingPoints::eOutputImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.addBinding(shaderio::BindingPoints::eAccumulationImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.addBinding(shaderio::BindingPoints::eMotionVectorsImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.addBinding(shaderio::BindingPoints::eNormalRoughnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.addBinding(shaderio::BindingPoints::eBaseColorMetalnessImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.addBinding(shaderio::BindingPoints::eViewZImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.addBinding(shaderio::BindingPoints::eDiffuseRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                      VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  bindings.addBinding(shaderio::BindingPoints::eSpecularRadianceHitDistanceImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                      VK_SHADER_STAGE_RAYGEN_BIT_KHR);

  const uint32_t frameSetCount = std::max(1u, m_App->getFrameCycleSize());
  NVVK_CHECK(m_DescPack.init(bindings, m_Allocator->getDevice(), frameSetCount, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                             VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT));
}

void PathTracer::CreatePipelineLayout()
{
  const VkPushConstantRange pushConstantRange{
      .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                    | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
      .offset     = 0,
      .size       = sizeof(shaderio::PathTracePushConstant),
  };

  NVVK_CHECK(nvvk::createPipelineLayout(m_Allocator->getDevice(), &m_PipelineLayout, {m_DescPack.getLayout()}, {pushConstantRange}));
}

void PathTracer::CreateRayTracingPipeline()
{
  VkDevice device                      = m_Allocator->getDevice();
  VkShaderModuleCreateInfo shaderCode = GetPathTracingShaderCode();

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

  std::array<VkPipelineShaderStageCreateInfo, eStageCount> stages{};
  for(VkPipelineShaderStageCreateInfo& stage : stages)
  {
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.pNext = &shaderCode;
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

  const uint32_t recursionDepth = std::max(1u, m_PipelineBounceLimit + 1);
  VkRayTracingPipelineCreateInfoKHR pipelineInfo{
      .sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
      .stageCount                   = static_cast<uint32_t>(stages.size()),
      .pStages                      = stages.data(),
      .groupCount                   = static_cast<uint32_t>(shaderGroups.size()),
      .pGroups                      = shaderGroups.data(),
      .maxPipelineRayRecursionDepth = recursionDepth,
      .layout                       = m_PipelineLayout,
  };

  NVVK_CHECK(vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline));

  m_SbtGenerator.init(device, m_RtProperties);
  const size_t sbtBufferSize = m_SbtGenerator.calculateSBTBufferSize(m_Pipeline, pipelineInfo);
  NVVK_CHECK(m_Allocator->createBuffer(m_SbtBuffer, sbtBufferSize,
                                       VK_BUFFER_USAGE_2_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
                                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                       VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                       m_SbtGenerator.getBufferAlignment()));
}

void PathTracer::CreateShaderBindingTable()
{
  NVVK_CHECK(m_SbtGenerator.populateSBTBuffer(m_SbtBuffer.address, m_SbtBuffer.bufferSize, m_SbtBuffer.mapping));
  m_SbtRegions = m_SbtGenerator.getSBTRegions(0);
}

void PathTracer::UpdateFrameDescriptors(const RenderInput& input)
{
  const uint32_t frameSetIndex = std::min(m_App->getFrameCycleIndex(), uint32_t(m_DescPack.getSets().size() - 1));

  nvvk::WriteSetContainer write;
  write.reserve(9);

  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eTlas, frameSetIndex), *input.topLevelAS);

  VkDescriptorImageInfo outputImageInfo = input.gBuffers->getDescriptorImageInfo(input.renderedImageIndex);
  outputImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eOutputImage, frameSetIndex), outputImageInfo);

  VkDescriptorImageInfo accumulationImageInfo = m_AccumulationImage.descriptor;
  accumulationImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eAccumulationImage, frameSetIndex), accumulationImageInfo);

  VkDescriptorImageInfo motionVectorsImageInfo = m_DenoiserResources.GetMotionVectorsImage().descriptor;
  motionVectorsImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eMotionVectorsImage, frameSetIndex), motionVectorsImageInfo);

  VkDescriptorImageInfo normalRoughnessImageInfo = m_DenoiserResources.GetNormalRoughnessImage().descriptor;
  normalRoughnessImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eNormalRoughnessImage, frameSetIndex), normalRoughnessImageInfo);

  VkDescriptorImageInfo baseColorMetalnessImageInfo = m_DenoiserResources.GetBaseColorMetalnessImage().descriptor;
  baseColorMetalnessImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eBaseColorMetalnessImage, frameSetIndex), baseColorMetalnessImageInfo);

  VkDescriptorImageInfo viewZImageInfo = m_DenoiserResources.GetViewZImage().descriptor;
  viewZImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eViewZImage, frameSetIndex), viewZImageInfo);

  VkDescriptorImageInfo diffuseRadianceHitDistanceImageInfo = m_DenoiserResources.GetDiffuseRadianceHitDistanceImage().descriptor;
  diffuseRadianceHitDistanceImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eDiffuseRadianceHitDistanceImage, frameSetIndex),
               diffuseRadianceHitDistanceImageInfo);

  VkDescriptorImageInfo specularRadianceHitDistanceImageInfo = m_DenoiserResources.GetSpecularRadianceHitDistanceImage().descriptor;
  specularRadianceHitDistanceImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eSpecularRadianceHitDistanceImage, frameSetIndex),
               specularRadianceHitDistanceImageInfo);

  vkUpdateDescriptorSets(m_Allocator->getDevice(), write.size(), write.data(), 0, nullptr);
}

void PathTracer::CreateOrResizeAccumulationImage(VkExtent2D size)
{
  if(m_AccumulationImage.image != VK_NULL_HANDLE && m_AccumulationImage.extent.width == size.width
     && m_AccumulationImage.extent.height == size.height)
  {
    return;
  }

  if(m_AccumulationImage.image != VK_NULL_HANDLE)
  {
    ScheduleAccumulationImageDestroy(m_AccumulationImage);
    m_AccumulationImage = {};
  }

  VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = kAccumulationFormat,
                              .extent = {.width = size.width, .height = size.height, .depth = 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = imageInfo.format,
                                 .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1,
                                                      .baseArrayLayer = 0, .layerCount = 1}};

  NVVK_CHECK(m_Allocator->createImage(m_AccumulationImage, imageInfo, viewInfo));
  m_AccumulationImage.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  m_AccumulationImage.descriptor.sampler     = VK_NULL_HANDLE;
  InvalidateAccumulation();
}

void PathTracer::DestroyAccumulationImage()
{
  m_Allocator->destroyImage(m_AccumulationImage);
  m_AccumulationImage = {};
}

void PathTracer::ScheduleAccumulationImageDestroy(nvvk::Image image)
{
  if(image.image == VK_NULL_HANDLE)
  {
    return;
  }

  nvvk::ResourceAllocator* allocator = m_Allocator;
  m_App->submitResourceFree([allocator, image]() mutable {
    if(allocator != nullptr)
    {
      allocator->destroyImage(image);
    }
  });
}

void PathTracer::TransitionStorageImageForWrite(VkCommandBuffer cmd, nvvk::Image& image, VkPipelineStageFlags2 dstStageMask)
{
  if(image.image == VK_NULL_HANDLE || image.descriptor.imageLayout == VK_IMAGE_LAYOUT_GENERAL)
  {
    return;
  }

  const VkImageMemoryBarrier2 transition{
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

  const VkDependencyInfo dependency{
      .sType                  = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &transition,
  };
  vkCmdPipelineBarrier2(cmd, &dependency);
  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

PathTracer::AccumulationSignature PathTracer::MakeAccumulationSignature(const RenderInput& input, VkExtent2D size) const
{
  AccumulationSignature signature{};
  signature.viewProjMatrix          = input.sceneInfo->viewProjMatrix;
  signature.projInvMatrix           = input.sceneInfo->projInvMatrix;
  signature.viewInvMatrix           = input.sceneInfo->viewInvMatrix;
  signature.cameraPosition          = input.sceneInfo->cameraPosition;
  signature.useSky                  = input.sceneInfo->useSky;
  signature.useHdrEnv               = input.sceneInfo->useHdrEnv;
  signature.environmentTextureIndex = input.sceneInfo->environmentTextureIndex;
  signature.backgroundColor         = input.sceneInfo->backgroundColor;
  signature.skySimpleParam          = input.sceneInfo->skySimpleParam;
  signature.topLevelAsAddress       = input.topLevelAS->address;
  signature.viewportSize            = size;
  return signature;
}

PathTracer::DenoiserSignature PathTracer::MakeDenoiserSignature(const RenderInput& input, VkExtent2D size) const
{
  DenoiserSignature signature{};
  signature.useSky                  = input.sceneInfo->useSky;
  signature.useHdrEnv               = input.sceneInfo->useHdrEnv;
  signature.environmentTextureIndex = input.sceneInfo->environmentTextureIndex;
  signature.backgroundColor         = input.sceneInfo->backgroundColor;
  signature.skySimpleParam          = input.sceneInfo->skySimpleParam;
  signature.topLevelAsAddress       = input.topLevelAS->address;
  signature.viewportSize            = size;
  return signature;
}

}  // namespace nvsamples
