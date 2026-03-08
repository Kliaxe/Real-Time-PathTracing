#include "PathTracer.h"

// Role:
// Holds the ray tracing pipeline objects and records the trace dispatch that
// writes the HDR render target directly through the ray tracing pipeline.

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include <nvapp/application.hpp>
#include <nvvk/barriers.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

#include "Common/Utils.hpp"

// Pre-compiled path tracing shader.
#include "_autogen/PathTracer.slang.h"

namespace nvsamples
{

namespace
{

constexpr uint32_t kRequestedMaxBounces = 3;

VkShaderModuleCreateInfo GetPathTracingShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(PathTracer_slang));
}

}  // namespace

PathTracer::PathTracer(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
    , m_MaxTextureDescriptors(createInfo.maxTextureDescriptors)
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
}

void PathTracer::Destroy()
{
  if(m_Allocator == nullptr)
  {
    return;
  }

  VkDevice device = m_Allocator->getDevice();

  m_Allocator->destroyBuffer(m_SbtBuffer);
  m_SbtBuffer = {};

  m_SbtGenerator.deinit();
  m_SbtRegions = {};

  vkDestroyPipeline(device, m_Pipeline, nullptr);
  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
  m_Pipeline       = VK_NULL_HANDLE;
  m_PipelineLayout = VK_NULL_HANDLE;

  m_DescPack.deinit();

  m_FrameNumber = 0;
  m_MaxBounces  = 0;
}

bool PathTracer::IsReady() const
{
  return m_Pipeline != VK_NULL_HANDLE && m_PipelineLayout != VK_NULL_HANDLE && m_SbtBuffer.buffer != VK_NULL_HANDLE;
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

  UpdateFrameDescriptors(input);

  const VkExtent2D size = input.gBuffers->getSize();
  if(size.width == 0 || size.height == 0)
  {
    return;
  }

  // Make the updated scene buffer contents visible to the ray tracing stages and
  // ensure the output image is ready for storage writes in GENERAL layout.
  nvvk::cmdMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);

  const VkImageMemoryBarrier2 outputBarrier{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
      .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .image         = input.gBuffers->getColorImage(input.renderedImageIndex),
      .subresourceRange = {
          .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
          .baseMipLevel   = 0,
          .levelCount     = 1,
          .baseArrayLayer = 0,
          .layerCount     = 1,
      },
  };
  const VkDependencyInfo outputDependency{
      .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount  = 1,
      .pImageMemoryBarriers     = &outputBarrier,
  };
  vkCmdPipelineBarrier2(input.cmd, &outputDependency);

  vkCmdBindPipeline(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_Pipeline);
  const uint32_t frameSetIndex = std::min(m_App->getFrameCycleIndex(), uint32_t(m_DescPack.getSets().size() - 1));
  vkCmdBindDescriptorSets(input.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1,
                          m_DescPack.getSetPtr(frameSetIndex), 0, nullptr);

  const shaderio::PathTracePushConstant pushConstant{
      .sceneInfoAddress = (shaderio::GltfSceneInfo*)input.sceneResource->bSceneInfo.address,
      .frameNumber      = m_FrameNumber++,
      .maxBounces       = m_MaxBounces,
      ._pad0            = 0,
  };
  vkCmdPushConstants(input.cmd, m_PipelineLayout,
                     VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 0,
                     sizeof(shaderio::PathTracePushConstant), &pushConstant);

  vkCmdTraceRaysKHR(input.cmd, &m_SbtRegions.raygen, &m_SbtRegions.miss, &m_SbtRegions.hit, &m_SbtRegions.callable, size.width,
                    size.height, 1);

  // Tonemapping runs immediately afterward as a compute pass, so we make the ray
  // tracing shader writes visible to compute reads here.
  nvvk::cmdMemoryBarrier(input.cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

void PathTracer::QueryRayTracingProperties()
{
  VkPhysicalDeviceProperties2 props{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &m_RtProperties,
  };
  vkGetPhysicalDeviceProperties2(m_Allocator->getPhysicalDevice(), &props);

  // Every extra bounce after the primary hit requires one more recursive TraceRay.
  // That means the shader-side bounce budget must stay below the pipeline's
  // maximum recursion depth minus the initial primary ray.
  m_MaxBounces = (m_RtProperties.maxRayRecursionDepth > 0)
                     ? std::min(kRequestedMaxBounces, m_RtProperties.maxRayRecursionDepth - 1)
                     : 0;
}

void PathTracer::CreateDescriptorSetLayout()
{
  const VkShaderStageFlags rayTracingStages =
      VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

  nvvk::DescriptorBindings bindings;
  bindings.addBinding({.binding         = shaderio::BindingPoints::eTextures,
                       .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       .descriptorCount = m_MaxTextureDescriptors,
                       .stageFlags      = rayTracingStages},
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                          | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
  bindings.addBinding(shaderio::BindingPoints::eTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, rayTracingStages);
  bindings.addBinding(shaderio::BindingPoints::eOutputImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR);

  // Allocate one descriptor set per frame-in-flight. This avoids rewriting a
  // descriptor set that may still be referenced by an older submitted command
  // buffer, which is exactly the hazard the validation error reported.
  const uint32_t frameSetCount = std::max(1u, m_App->getFrameCycleSize());
  NVVK_CHECK(m_DescPack.init(bindings, m_Allocator->getDevice(), frameSetCount, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                             VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT));
  NVVK_DBG_NAME(m_DescPack.getLayout());
  NVVK_DBG_NAME(m_DescPack.getPool());
  NVVK_DBG_NAME(m_DescPack.getSet(0));
}

void PathTracer::CreatePipelineLayout()
{
  const VkPushConstantRange pushConstantRange{
      .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
      .offset     = 0,
      .size       = sizeof(shaderio::PathTracePushConstant),
  };

  NVVK_CHECK(nvvk::createPipelineLayout(m_Allocator->getDevice(), &m_PipelineLayout, {m_DescPack.getLayout()}, {pushConstantRange}));
  NVVK_DBG_NAME(m_PipelineLayout);
}

void PathTracer::CreateRayTracingPipeline()
{
  VkDevice device = m_Allocator->getDevice();
  VkShaderModuleCreateInfo shaderCode = GetPathTracingShaderCode();

  enum StageIndices
  {
    eRaygen,
    eMiss,
    eClosestHit,
    eShaderGroupCount,
  };

  std::array<VkPipelineShaderStageCreateInfo, eShaderGroupCount> stages{};
  for(VkPipelineShaderStageCreateInfo& stage : stages)
  {
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.pNext = &shaderCode;
  }

  stages[eRaygen].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  stages[eRaygen].pName = "rgenMain";

  stages[eMiss].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
  stages[eMiss].pName = "rmissMain";

  stages[eClosestHit].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
  stages[eClosestHit].pName = "rchitMain";

  VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
  group.anyHitShader       = VK_SHADER_UNUSED_KHR;
  group.closestHitShader   = VK_SHADER_UNUSED_KHR;
  group.generalShader      = VK_SHADER_UNUSED_KHR;
  group.intersectionShader = VK_SHADER_UNUSED_KHR;

  std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups;
  shaderGroups.reserve(3);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eRaygen;
  shaderGroups.push_back(group);

  group.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  group.generalShader = eMiss;
  shaderGroups.push_back(group);

  group.type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  group.generalShader    = VK_SHADER_UNUSED_KHR;
  group.closestHitShader = eClosestHit;
  shaderGroups.push_back(group);

  const uint32_t recursionDepth = std::max(1u, m_MaxBounces + 1);

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
  NVVK_DBG_NAME(m_Pipeline);

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
  m_SbtRegions = m_SbtGenerator.getSBTRegions();
}

void PathTracer::UpdateFrameDescriptors(const RenderInput& input)
{
  const uint32_t frameSetIndex = std::min(m_App->getFrameCycleIndex(), uint32_t(m_DescPack.getSets().size() - 1));

  nvvk::WriteSetContainer write;
  write.reserve(2);

  // Only touch the descriptor set that belongs to the frame we are currently
  // recording. Older in-flight frames keep their own descriptor contents until
  // the frame ring comes back around to them.
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eTlas, frameSetIndex), *input.topLevelAS);

  VkDescriptorImageInfo outputImageInfo = input.gBuffers->getDescriptorImageInfo(input.renderedImageIndex);
  outputImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
  write.append(m_DescPack.makeWrite(shaderio::BindingPoints::eOutputImage, frameSetIndex), outputImageInfo);

  vkUpdateDescriptorSets(m_Allocator->getDevice(), write.size(), write.data(), 0, nullptr);
}

}  // namespace nvsamples

