#include "PathTraceNrdDenoiser.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <span>
#include <vector>

#include <glm/gtc/type_ptr.hpp>
#include <nvapp/application.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>

#include "Common/Utils.hpp"

#ifdef THESIS_ENABLE_NRD
#include "_autogen/PathTraceNrdCompose.slang.h"
#endif

namespace nvsamples
{

namespace
{

constexpr VkFormat kDenoisedRadianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kComposeGroupSize       = 8;

template <typename T>
T AlignTo(T value, T alignment)
{
  return (value + alignment - 1) / alignment * alignment;
}

uint32_t DivideUp(uint32_t x, uint16_t y)
{
  return (x + y - 1u) / y;
}

#ifdef THESIS_ENABLE_NRD
VkShaderModuleCreateInfo GetComposeShaderCode()
{
  return nvsamples::GetShaderModuleCreateInfo(std::span(PathTraceNrdCompose_slang));
}

bool IsNrdSuccess(nrd::Result result)
{
  return result == nrd::Result::SUCCESS;
}
#endif

}  // namespace

PathTraceNrdDenoiser::PathTraceNrdDenoiser(const CreateInfo& createInfo)
    : m_App(createInfo.app)
    , m_Allocator(createInfo.allocator)
{
}

void PathTraceNrdDenoiser::Initialize()
{
#ifndef THESIS_ENABLE_NRD
  return;
#else
  if(m_App == nullptr || m_Allocator == nullptr || m_Instance != nullptr)
  {
    return;
  }

  m_LibraryDesc = nrd::GetLibraryDesc();

  const nrd::DenoiserDesc denoiserDesc{
      .identifier = kDenoiserIdentifier,
      .denoiser   = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR,
  };
  const nrd::InstanceCreationDesc instanceCreateInfo{
      .denoisers   = &denoiserDesc,
      .denoisersNum = 1,
  };

  if(!IsNrdSuccess(nrd::CreateInstance(instanceCreateInfo, m_Instance)) || m_Instance == nullptr)
  {
    m_Instance = nullptr;
    return;
  }

  m_InstanceDesc = nrd::GetInstanceDesc(*m_Instance);

  VkPhysicalDeviceProperties2 properties{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
  };
  vkGetPhysicalDeviceProperties2(m_Allocator->getPhysicalDevice(), &properties);
  m_ConstantBufferAlignment = std::max(1u, static_cast<uint32_t>(properties.properties.limits.minUniformBufferOffsetAlignment));

  m_ReblurSettings.maxAccumulatedFrameNum      = 30;
  m_ReblurSettings.maxFastAccumulatedFrameNum  = 6;
  m_ReblurSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;

  CreateVulkanState();
#endif
}

void PathTraceNrdDenoiser::Destroy()
{
#ifndef THESIS_ENABLE_NRD
  return;
#else
  if(m_Allocator == nullptr)
  {
    return;
  }

  DestroyVulkanState();

  if(m_Instance != nullptr)
  {
    nrd::DestroyInstance(*m_Instance);
  }

  m_LibraryDesc            = nullptr;
  m_InstanceDesc           = nullptr;
  m_Instance               = nullptr;
  m_HasPreviousMatrices    = false;
  m_PreviousViewMatrix     = glm::mat4(1.0f);
  m_PreviousProjectionMatrix = glm::mat4(1.0f);
  m_FrameIndex             = 0;
#endif
}

bool PathTraceNrdDenoiser::IsReady() const
{
#ifndef THESIS_ENABLE_NRD
  return false;
#else
  return m_Instance != nullptr && m_PipelineLayout != VK_NULL_HANDLE && !m_Pipelines.empty() && m_ComposePipeline != VK_NULL_HANDLE;
#endif
}

void PathTraceNrdDenoiser::InvalidateHistory()
{
#ifdef THESIS_ENABLE_NRD
  m_HistoryInvalidated   = true;
  m_HasPreviousMatrices  = false;
  m_FrameIndex           = 0;
#endif
}

void PathTraceNrdDenoiser::PrepareFrame(const FrameInput& input, const PathTraceDenoiserResources& denoiserInputs)
{
#ifndef THESIS_ENABLE_NRD
  (void)input;
  (void)denoiserInputs;
  return;
#else
  if(!IsReady() || input.sceneInfo == nullptr || input.viewportSize.width == 0 || input.viewportSize.height == 0)
  {
    return;
  }

  (void)denoiserInputs;
  EnsureForViewport(input.viewportSize);
  m_EnableMaterialDemodulation = input.enableMaterialDemodulation;
  if(input.settings != nullptr)
  {
    ApplyDenoiserSettings(*input.settings);
  }
  m_HistoryInvalidated = m_HistoryInvalidated || input.historyInvalidated;
  if(m_HistoryInvalidated)
  {
    m_FrameIndex = 0;
  }
  UpdateCommonSettings(input);

  assert(IsNrdSuccess(nrd::SetCommonSettings(*m_Instance, m_CommonSettings)));
  assert(IsNrdSuccess(nrd::SetDenoiserSettings(*m_Instance, kDenoiserIdentifier, &m_ReblurSettings)));

  m_HistoryInvalidated = false;
#endif
}

void PathTraceNrdDenoiser::ApplyDenoiserSettings(const DenoiserSettings& settings)
{
#ifdef THESIS_ENABLE_NRD
  m_ReblurSettings.maxAccumulatedFrameNum      = settings.maxAccumulatedFrames;
  m_ReblurSettings.maxFastAccumulatedFrameNum  = std::min(settings.maxFastAccumulatedFrames, settings.maxAccumulatedFrames);
  m_ReblurSettings.diffusePrepassBlurRadius    = settings.diffusePrepassBlurRadius;
  m_ReblurSettings.specularPrepassBlurRadius   = settings.specularPrepassBlurRadius;
  m_ReblurSettings.enableAntiFirefly           = settings.enableAntiFirefly;
#else
  (void)settings;
#endif
}

void PathTraceNrdDenoiser::Denoise(VkCommandBuffer cmd,
                                   const PathTraceDenoiserResources& denoiserInputs,
                                   VkImageView rawBeautyImageView,
                                   VkImageView outputImageView,
                                   DenoiserDebugView debugView,
                                   VkExtent2D viewportSize)
{
#ifndef THESIS_ENABLE_NRD
  (void)cmd;
  (void)denoiserInputs;
  (void)rawBeautyImageView;
  (void)outputImageView;
  (void)debugView;
  (void)viewportSize;
  return;
#else
  if(!IsReady() || cmd == VK_NULL_HANDLE || rawBeautyImageView == VK_NULL_HANDLE || outputImageView == VK_NULL_HANDLE
     || viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  EnsureForViewport(viewportSize);

  TransitionImageToGeneral(cmd, const_cast<nvvk::Image&>(denoiserInputs.GetMotionVectorsImage()), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<nvvk::Image&>(denoiserInputs.GetNormalRoughnessImage()), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<nvvk::Image&>(denoiserInputs.GetBaseColorMetalnessImage()),
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<nvvk::Image&>(denoiserInputs.GetViewZImage()), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<nvvk::Image&>(denoiserInputs.GetDiffuseRadianceHitDistanceImage()),
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<nvvk::Image&>(denoiserInputs.GetSpecularRadianceHitDistanceImage()),
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, m_DiffuseOutputImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, m_SpecularOutputImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  for(nvvk::Image& image : m_PermanentPoolImages)
  {
    TransitionImageToGeneral(cmd, image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  }

  for(nvvk::Image& image : m_TransientPoolImages)
  {
    TransitionImageToGeneral(cmd, image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  }

  FrameResources& frameResources = GetCurrentFrameResources();
  BeginFrame(frameResources);
  UpdateFrameSet(frameResources);
  DispatchNrd(cmd, frameResources, denoiserInputs);
  ComposeDenoisedResult(cmd, frameResources, denoiserInputs, rawBeautyImageView, outputImageView, debugView, viewportSize);
#endif
}

const nvvk::Image& PathTraceNrdDenoiser::GetDiffuseOutputImage() const
{
  return m_DiffuseOutputImage;
}

const nvvk::Image& PathTraceNrdDenoiser::GetSpecularOutputImage() const
{
  return m_SpecularOutputImage;
}

void PathTraceNrdDenoiser::CreateVulkanState()
{
#ifndef THESIS_ENABLE_NRD
  return;
#else
  CreateSamplers();
  CreateDescriptorSetLayouts();
  CreatePipelineLayout();
  CreatePipelines();
  CreateFrameResources();
#endif
}

void PathTraceNrdDenoiser::DestroyVulkanState()
{
  if(m_Allocator == nullptr)
  {
    return;
  }

  VkDevice device = m_Allocator->getDevice();

  DestroyViewportResources(false);
  DestroyFrameResources();

  for(VkPipeline pipeline : m_Pipelines)
  {
    vkDestroyPipeline(device, pipeline, nullptr);
  }
  m_Pipelines.clear();

  vkDestroyPipeline(device, m_ComposePipeline, nullptr);
  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
  vkDestroyPipelineLayout(device, m_ComposePipelineLayout, nullptr);
  vkDestroyDescriptorSetLayout(device, m_ResourceSetLayout, nullptr);
  vkDestroyDescriptorSetLayout(device, m_FrameSetLayout, nullptr);
  vkDestroyDescriptorSetLayout(device, m_ComposeSetLayout, nullptr);
  vkDestroySampler(device, m_NearestSampler, nullptr);
  vkDestroySampler(device, m_LinearSampler, nullptr);

  m_ComposePipeline       = VK_NULL_HANDLE;
  m_PipelineLayout        = VK_NULL_HANDLE;
  m_ComposePipelineLayout = VK_NULL_HANDLE;
  m_ResourceSetLayout     = VK_NULL_HANDLE;
  m_FrameSetLayout        = VK_NULL_HANDLE;
  m_ComposeSetLayout      = VK_NULL_HANDLE;
  m_NearestSampler        = VK_NULL_HANDLE;
  m_LinearSampler         = VK_NULL_HANDLE;
}

void PathTraceNrdDenoiser::CreateSamplers()
{
#ifndef THESIS_ENABLE_NRD
  return;
#else
  VkDevice device = m_Allocator->getDevice();

  const VkSamplerCreateInfo nearestSamplerInfo{
      .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter    = VK_FILTER_NEAREST,
      .minFilter    = VK_FILTER_NEAREST,
      .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod       = VK_LOD_CLAMP_NONE,
  };
  NVVK_CHECK(vkCreateSampler(device, &nearestSamplerInfo, nullptr, &m_NearestSampler));

  const VkSamplerCreateInfo linearSamplerInfo{
      .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter    = VK_FILTER_LINEAR,
      .minFilter    = VK_FILTER_LINEAR,
      .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod       = VK_LOD_CLAMP_NONE,
  };
  NVVK_CHECK(vkCreateSampler(device, &linearSamplerInfo, nullptr, &m_LinearSampler));
#endif
}

void PathTraceNrdDenoiser::CreateDescriptorSetLayouts()
{
#ifndef THESIS_ENABLE_NRD
  return;
#else
  VkDevice device = m_Allocator->getDevice();

  const uint32_t textureBinding =
      m_LibraryDesc->spirvBindingOffsets.textureOffset + m_InstanceDesc->resourcesBaseRegisterIndex;
  const uint32_t storageBinding =
      m_LibraryDesc->spirvBindingOffsets.storageTextureAndBufferOffset + m_InstanceDesc->resourcesBaseRegisterIndex;
  const uint32_t samplerBinding =
      m_LibraryDesc->spirvBindingOffsets.samplerOffset + m_InstanceDesc->samplersBaseRegisterIndex;
  const uint32_t constantBufferBinding =
      m_LibraryDesc->spirvBindingOffsets.constantBufferOffset + m_InstanceDesc->constantBufferRegisterIndex;

  std::vector<VkDescriptorSetLayoutBinding> resourceBindings;
  std::vector<VkDescriptorBindingFlags>     resourceBindingFlags;
  resourceBindings.reserve(m_InstanceDesc->descriptorPoolDesc.perSetTexturesMaxNum
                           + m_InstanceDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum);
  resourceBindingFlags.reserve(resourceBindings.capacity());

  for(uint32_t textureIndex = 0; textureIndex < m_InstanceDesc->descriptorPoolDesc.perSetTexturesMaxNum; ++textureIndex)
  {
    resourceBindings.push_back(VkDescriptorSetLayoutBinding{
        .binding         = textureBinding + textureIndex,
        .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
    });
    resourceBindingFlags.push_back(0);
  }

  for(uint32_t storageIndex = 0; storageIndex < m_InstanceDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum; ++storageIndex)
  {
    resourceBindings.push_back(VkDescriptorSetLayoutBinding{
        .binding         = storageBinding + storageIndex,
        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
    });
    resourceBindingFlags.push_back(0);
  }

  const VkDescriptorSetLayoutBindingFlagsCreateInfo resourceBindingFlagsInfo{
      .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
      .bindingCount  = static_cast<uint32_t>(resourceBindingFlags.size()),
      .pBindingFlags = resourceBindingFlags.data(),
  };
  const VkDescriptorSetLayoutCreateInfo resourceLayoutInfo{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext        = &resourceBindingFlagsInfo,
      .bindingCount = static_cast<uint32_t>(resourceBindings.size()),
      .pBindings    = resourceBindings.data(),
  };
  NVVK_CHECK(vkCreateDescriptorSetLayout(device, &resourceLayoutInfo, nullptr, &m_ResourceSetLayout));

  const VkSampler samplers[] = {m_NearestSampler, m_LinearSampler};
  assert(m_InstanceDesc->samplersNum <= std::size(samplers));
  std::vector<VkDescriptorSetLayoutBinding> frameBindings;
  frameBindings.reserve(m_InstanceDesc->samplersNum + 1);
  for(uint32_t samplerIndex = 0; samplerIndex < m_InstanceDesc->samplersNum; ++samplerIndex)
  {
    frameBindings.push_back(VkDescriptorSetLayoutBinding{
        .binding            = samplerBinding + samplerIndex,
        .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
        .descriptorCount    = 1,
        .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = &samplers[samplerIndex],
    });
  }
  frameBindings.push_back(VkDescriptorSetLayoutBinding{
      .binding         = constantBufferBinding,
      .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
      .descriptorCount = 1,
      .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
  });

  const VkDescriptorSetLayoutCreateInfo frameLayoutInfo{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(frameBindings.size()),
      .pBindings    = frameBindings.data(),
  };
  NVVK_CHECK(vkCreateDescriptorSetLayout(device, &frameLayoutInfo, nullptr, &m_FrameSetLayout));

  const std::array<VkDescriptorSetLayoutBinding, 10> composeBindings{{
      VkDescriptorSetLayoutBinding{
          .binding         = 0,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding{
          .binding         = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding{
          .binding         = 2,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding{
          .binding         = 3,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding{
          .binding         = 4,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding{
          .binding         = 5,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding{
          .binding         = 6,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding{
          .binding         = 7,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding{
          .binding         = 8,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding{
          .binding         = 9,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
  }};
  const VkDescriptorSetLayoutCreateInfo composeLayoutInfo{
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(composeBindings.size()),
      .pBindings    = composeBindings.data(),
  };
  NVVK_CHECK(vkCreateDescriptorSetLayout(device, &composeLayoutInfo, nullptr, &m_ComposeSetLayout));
#endif
}

void PathTraceNrdDenoiser::CreatePipelineLayout()
{
  VkDevice device = m_Allocator->getDevice();

  const std::array<VkDescriptorSetLayout, 2> setLayouts{
      m_ResourceSetLayout,
      m_FrameSetLayout,
  };
  const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
      .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
      .pSetLayouts    = setLayouts.data(),
  };
  NVVK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout));

  struct ComposePushConstants
  {
    uint32_t debugView = 0;
    uint32_t useMaterialDemodulation = 0;
  };
  const VkPushConstantRange composePushConstantRange{
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset     = 0,
      .size       = sizeof(ComposePushConstants),
  };
  const VkPipelineLayoutCreateInfo composePipelineLayoutInfo{
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = &m_ComposeSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges    = &composePushConstantRange,
  };
  NVVK_CHECK(vkCreatePipelineLayout(device, &composePipelineLayoutInfo, nullptr, &m_ComposePipelineLayout));
}

void PathTraceNrdDenoiser::CreatePipelines()
{
#ifndef THESIS_ENABLE_NRD
  return;
#else
  VkDevice device = m_Allocator->getDevice();
  m_Pipelines.reserve(m_InstanceDesc->pipelinesNum);

  for(uint32_t pipelineIndex = 0; pipelineIndex < m_InstanceDesc->pipelinesNum; ++pipelineIndex)
  {
    const nrd::PipelineDesc& pipelineDesc = m_InstanceDesc->pipelines[pipelineIndex];
    const VkShaderModuleCreateInfo shaderCode{
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = static_cast<size_t>(pipelineDesc.computeShaderSPIRV.size),
        .pCode    = static_cast<const uint32_t*>(pipelineDesc.computeShaderSPIRV.bytecode),
    };

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    NVVK_CHECK(vkCreateShaderModule(device, &shaderCode, nullptr, &shaderModule));

    const VkPipelineShaderStageCreateInfo shaderStage{
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shaderModule,
        .pName  = m_InstanceDesc->shaderEntryPoint,
    };
    const VkComputePipelineCreateInfo pipelineInfo{
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = shaderStage,
        .layout = m_PipelineLayout,
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    NVVK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    m_Pipelines.push_back(pipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
  }

  const VkShaderModuleCreateInfo composeShaderCode = GetComposeShaderCode();
  VkShaderModule                 composeShaderModule = VK_NULL_HANDLE;
  NVVK_CHECK(vkCreateShaderModule(device, &composeShaderCode, nullptr, &composeShaderModule));

  const VkPipelineShaderStageCreateInfo composeShaderStage{
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = composeShaderModule,
      .pName  = "main",
  };
  const VkComputePipelineCreateInfo composePipelineInfo{
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = composeShaderStage,
      .layout = m_ComposePipelineLayout,
  };
  NVVK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &composePipelineInfo, nullptr, &m_ComposePipeline));
  vkDestroyShaderModule(device, composeShaderModule, nullptr);
#endif
}

void PathTraceNrdDenoiser::CreateFrameResources()
{
#ifndef THESIS_ENABLE_NRD
  return;
#else
  const uint32_t frameCount = std::max(1u, m_App->getFrameCycleSize());
  m_FrameResources.resize(frameCount);

  for(FrameResources& frameResources : m_FrameResources)
  {
    const VkDescriptorPoolSize poolSizes[] = {
        VkDescriptorPoolSize{
            .type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = m_InstanceDesc->descriptorPoolDesc.setsMaxNum * m_InstanceDesc->descriptorPoolDesc.perSetTexturesMaxNum + 2,
        },
        VkDescriptorPoolSize{
            .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = m_InstanceDesc->descriptorPoolDesc.setsMaxNum * m_InstanceDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum + 1,
        },
        VkDescriptorPoolSize{
            .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .descriptorCount = 1,
        },
    };
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = m_InstanceDesc->descriptorPoolDesc.setsMaxNum + 2,
        .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
        .pPoolSizes    = poolSizes,
    };
    NVVK_CHECK(vkCreateDescriptorPool(m_Allocator->getDevice(), &poolInfo, nullptr, &frameResources.descriptorPool));

    const uint32_t constantBufferSize =
        std::max(1u, m_InstanceDesc->descriptorPoolDesc.setsMaxNum) * AlignTo(m_InstanceDesc->constantBufferMaxDataSize, m_ConstantBufferAlignment);
    NVVK_CHECK(m_Allocator->createBuffer(frameResources.constantBuffer, constantBufferSize, VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT,
                                         VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                         VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT));
  }
#endif
}

void PathTraceNrdDenoiser::DestroyFrameResources()
{
  if(m_Allocator == nullptr)
  {
    return;
  }

  VkDevice device = m_Allocator->getDevice();
  for(FrameResources& frameResources : m_FrameResources)
  {
    m_Allocator->destroyBuffer(frameResources.constantBuffer);
    vkDestroyDescriptorPool(device, frameResources.descriptorPool, nullptr);
    frameResources = {};
  }

  m_FrameResources.clear();
}

void PathTraceNrdDenoiser::DestroyViewportResources(bool deferDestruction)
{
  auto releaseImage = [&](nvvk::Image& image) {
    if(image.image == VK_NULL_HANDLE)
    {
      return;
    }

    if(deferDestruction)
    {
      ScheduleImageDestroy(image);
    }
    else
    {
      m_Allocator->destroyImage(image);
    }
  };

  releaseImage(m_DiffuseOutputImage);
  releaseImage(m_SpecularOutputImage);

  for(nvvk::Image& image : m_PermanentPoolImages)
  {
    releaseImage(image);
  }
  for(nvvk::Image& image : m_TransientPoolImages)
  {
    releaseImage(image);
  }

  m_DiffuseOutputImage  = {};
  m_SpecularOutputImage = {};
  m_PermanentPoolImages.clear();
  m_TransientPoolImages.clear();
  m_ViewportSize = {};
}

void PathTraceNrdDenoiser::ScheduleImageDestroy(nvvk::Image image)
{
  if(image.image == VK_NULL_HANDLE || m_App == nullptr || m_Allocator == nullptr)
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

void PathTraceNrdDenoiser::EnsureForViewport(VkExtent2D viewportSize)
{
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height
     && m_DiffuseOutputImage.image != VK_NULL_HANDLE)
  {
    return;
  }

  RecreateViewportResources(viewportSize);
}

void PathTraceNrdDenoiser::RecreateViewportResources(VkExtent2D viewportSize)
{
  DestroyViewportResources(true);

  m_ViewportSize       = viewportSize;
  m_HistoryInvalidated = true;

  m_DiffuseOutputImage  = CreateStorageImage(viewportSize, kDenoisedRadianceFormat, "PathTraceNrdDiffuseOutput");
  m_SpecularOutputImage = CreateStorageImage(viewportSize, kDenoisedRadianceFormat, "PathTraceNrdSpecularOutput");

#ifdef THESIS_ENABLE_NRD
  m_PermanentPoolImages.reserve(m_InstanceDesc->permanentPoolSize);
  for(uint32_t imageIndex = 0; imageIndex < m_InstanceDesc->permanentPoolSize; ++imageIndex)
  {
    const VkExtent2D imageSize{
        .width  = DivideUp(viewportSize.width, m_InstanceDesc->permanentPool[imageIndex].downsampleFactor),
        .height = DivideUp(viewportSize.height, m_InstanceDesc->permanentPool[imageIndex].downsampleFactor),
    };
    m_PermanentPoolImages.push_back(CreateStorageImage(imageSize, ToVkFormat(m_InstanceDesc->permanentPool[imageIndex].format),
                                                       "PathTraceNrdPermanentPool"));
  }

  m_TransientPoolImages.reserve(m_InstanceDesc->transientPoolSize);
  for(uint32_t imageIndex = 0; imageIndex < m_InstanceDesc->transientPoolSize; ++imageIndex)
  {
    const VkExtent2D imageSize{
        .width  = DivideUp(viewportSize.width, m_InstanceDesc->transientPool[imageIndex].downsampleFactor),
        .height = DivideUp(viewportSize.height, m_InstanceDesc->transientPool[imageIndex].downsampleFactor),
    };
    m_TransientPoolImages.push_back(CreateStorageImage(imageSize, ToVkFormat(m_InstanceDesc->transientPool[imageIndex].format),
                                                       "PathTraceNrdTransientPool"));
  }
#endif
}

nvvk::Image PathTraceNrdDenoiser::CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const
{
  nvvk::Image image;

  VkImageCreateInfo imageInfo{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = format,
      .extent        = {.width = viewportSize.width, .height = viewportSize.height, .depth = 1},
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VkImageViewCreateInfo viewInfo{
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = format,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
  };

  NVVK_CHECK(m_Allocator->createImage(image, imageInfo, viewInfo));
  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image.descriptor.sampler     = VK_NULL_HANDLE;
  (void)debugName;
  NVVK_DBG_NAME(image.image);
  NVVK_DBG_NAME(image.descriptor.imageView);
  return image;
}

void PathTraceNrdDenoiser::UpdateCommonSettings(const FrameInput& input)
{
#ifndef THESIS_ENABLE_NRD
  (void)input;
  return;
#else
  const shaderio::GltfSceneInfo& sceneInfo = *input.sceneInfo;

  const glm::mat4 currentViewMatrix       = sceneInfo.viewMatrix;
  const glm::mat4 currentProjectionMatrix = sceneInfo.viewProjMatrix * sceneInfo.viewInvMatrix;
  const glm::mat4 previousViewMatrix      = m_HasPreviousMatrices ? m_PreviousViewMatrix : currentViewMatrix;
  const glm::mat4 previousProjectionMatrix =
      m_HasPreviousMatrices ? m_PreviousProjectionMatrix : currentProjectionMatrix;

  CopyMatrix(currentProjectionMatrix, m_CommonSettings.viewToClipMatrix);
  CopyMatrix(previousProjectionMatrix, m_CommonSettings.viewToClipMatrixPrev);
  CopyMatrix(currentViewMatrix, m_CommonSettings.worldToViewMatrix);
  CopyMatrix(previousViewMatrix, m_CommonSettings.worldToViewMatrixPrev);

  m_CommonSettings.motionVectorScale[0] = 1.0f;
  m_CommonSettings.motionVectorScale[1] = 1.0f;
  m_CommonSettings.motionVectorScale[2] = 1.0f;
  m_CommonSettings.cameraJitter[0]      = 0.0f;
  m_CommonSettings.cameraJitter[1]      = 0.0f;
  m_CommonSettings.cameraJitterPrev[0]  = 0.0f;
  m_CommonSettings.cameraJitterPrev[1]  = 0.0f;
  m_CommonSettings.resourceSize[0]      = static_cast<uint16_t>(input.viewportSize.width);
  m_CommonSettings.resourceSize[1]      = static_cast<uint16_t>(input.viewportSize.height);
  m_CommonSettings.resourceSizePrev[0]  = static_cast<uint16_t>(input.viewportSize.width);
  m_CommonSettings.resourceSizePrev[1]  = static_cast<uint16_t>(input.viewportSize.height);
  m_CommonSettings.rectSize[0]          = static_cast<uint16_t>(input.viewportSize.width);
  m_CommonSettings.rectSize[1]          = static_cast<uint16_t>(input.viewportSize.height);
  m_CommonSettings.rectSizePrev[0]      = static_cast<uint16_t>(input.viewportSize.width);
  m_CommonSettings.rectSizePrev[1]      = static_cast<uint16_t>(input.viewportSize.height);
  m_CommonSettings.viewZScale           = 1.0f;
  m_CommonSettings.denoisingRange       = 500000.0f;
  m_CommonSettings.disocclusionThreshold = 0.01f;
  m_CommonSettings.disocclusionThresholdAlternate = 0.05f;
  m_CommonSettings.splitScreen          = 0.0f;
  m_CommonSettings.frameIndex           = m_FrameIndex;
  m_CommonSettings.accumulationMode =
      m_HistoryInvalidated ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
  m_CommonSettings.isMotionVectorInWorldSpace        = false;
  m_CommonSettings.isHistoryConfidenceAvailable      = false;
  m_CommonSettings.isDisocclusionThresholdMixAvailable = false;
  m_CommonSettings.isBaseColorMetalnessAvailable     = m_EnableMaterialDemodulation;
  m_CommonSettings.enableValidation                  = false;

  m_PreviousViewMatrix       = currentViewMatrix;
  m_PreviousProjectionMatrix = currentProjectionMatrix;
  m_HasPreviousMatrices      = true;
  m_FrameIndex += 1;
#endif
}

PathTraceNrdDenoiser::FrameResources& PathTraceNrdDenoiser::GetCurrentFrameResources()
{
  assert(!m_FrameResources.empty());
  const uint32_t frameIndex = std::min(m_App->getFrameCycleIndex(), uint32_t(m_FrameResources.size() - 1));
  return m_FrameResources[frameIndex];
}

void PathTraceNrdDenoiser::BeginFrame(FrameResources& frameResources)
{
  frameResources.constantBufferOffset        = 0;
  frameResources.previousConstantBufferOffset = 0;
  frameResources.frameSet                    = VK_NULL_HANDLE;

  NVVK_CHECK(vkResetDescriptorPool(m_Allocator->getDevice(), frameResources.descriptorPool, 0));
  frameResources.frameSet = AllocateDescriptorSet(frameResources, m_FrameSetLayout);
}

void PathTraceNrdDenoiser::UpdateFrameSet(FrameResources& frameResources)
{
#ifndef THESIS_ENABLE_NRD
  (void)frameResources;
  return;
#else
  const VkDescriptorBufferInfo constantBufferInfo{
      .buffer = frameResources.constantBuffer.buffer,
      .offset = 0,
      .range  = m_InstanceDesc->constantBufferMaxDataSize,
  };
  const VkWriteDescriptorSet write{
      .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet          = frameResources.frameSet,
      .dstBinding      = m_LibraryDesc->spirvBindingOffsets.constantBufferOffset + m_InstanceDesc->constantBufferRegisterIndex,
      .descriptorCount = 1,
      .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
      .pBufferInfo     = &constantBufferInfo,
  };
  vkUpdateDescriptorSets(m_Allocator->getDevice(), 1, &write, 0, nullptr);
#endif
}

VkDescriptorSet PathTraceNrdDenoiser::AllocateDescriptorSet(FrameResources& frameResources, VkDescriptorSetLayout layout)
{
  const VkDescriptorSetAllocateInfo allocInfo{
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = frameResources.descriptorPool,
      .descriptorSetCount = 1,
      .pSetLayouts        = &layout,
  };

  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  NVVK_CHECK(vkAllocateDescriptorSets(m_Allocator->getDevice(), &allocInfo, &descriptorSet));
  return descriptorSet;
}

uint32_t PathTraceNrdDenoiser::UploadConstantData(FrameResources& frameResources,
                                                  const void*     constantData,
                                                  uint32_t        constantDataSize,
                                                  bool            reusePreviousData)
{
#ifndef THESIS_ENABLE_NRD
  (void)frameResources;
  (void)constantData;
  (void)constantDataSize;
  (void)reusePreviousData;
  return 0;
#else
  if(constantDataSize == 0 || constantData == nullptr)
  {
    return 0;
  }

  if(reusePreviousData)
  {
    return frameResources.previousConstantBufferOffset;
  }

  const uint32_t alignedConstantDataSize = AlignTo(constantDataSize, m_ConstantBufferAlignment);
  if(frameResources.constantBufferOffset + alignedConstantDataSize > frameResources.constantBuffer.bufferSize)
  {
    frameResources.constantBufferOffset = 0;
  }

  const uint32_t currentOffset = frameResources.constantBufferOffset;
  std::memcpy(frameResources.constantBuffer.mapping + currentOffset, constantData, constantDataSize);
  NVVK_CHECK(m_Allocator->autoFlushBuffer(frameResources.constantBuffer, currentOffset, constantDataSize));

  frameResources.constantBufferOffset         += alignedConstantDataSize;
  frameResources.previousConstantBufferOffset  = currentOffset;
  return currentOffset;
#endif
}

void PathTraceNrdDenoiser::UpdateResourceSet(VkDescriptorSet                      resourceSet,
                                             const nrd::DispatchDesc&            dispatchDesc,
                                             const PathTraceDenoiserResources&   denoiserInputs)
{
#ifndef THESIS_ENABLE_NRD
  (void)resourceSet;
  (void)dispatchDesc;
  (void)denoiserInputs;
  return;
#else
  const nrd::PipelineDesc& pipelineDesc = m_InstanceDesc->pipelines[dispatchDesc.pipelineIndex];

  std::vector<VkDescriptorImageInfo> imageInfos;
  std::vector<VkWriteDescriptorSet>  writes;
  imageInfos.reserve(dispatchDesc.resourcesNum);
  writes.reserve(dispatchDesc.resourcesNum);

  uint32_t resourceIndex = 0;
  uint32_t sampledBindingIndex = 0;
  uint32_t storageBindingIndex = 0;
  for(uint32_t rangeIndex = 0; rangeIndex < pipelineDesc.resourceRangesNum; ++rangeIndex)
  {
    const nrd::ResourceRangeDesc& resourceRange = pipelineDesc.resourceRanges[rangeIndex];
    for(uint32_t descriptorIndex = 0; descriptorIndex < resourceRange.descriptorsNum; ++descriptorIndex)
    {
      const nrd::ResourceDesc& resourceDesc = dispatchDesc.resources[resourceIndex++];
      const nvvk::Image&       image = ResolveDispatchImage(resourceDesc.type, resourceDesc.indexInPool, denoiserInputs);

      VkDescriptorImageInfo descriptorImageInfo{
          .sampler     = VK_NULL_HANDLE,
          .imageView   = image.descriptor.imageView,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
      imageInfos.push_back(descriptorImageInfo);

      uint32_t binding = 0;
      VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      if(resourceRange.descriptorType == nrd::DescriptorType::TEXTURE)
      {
        binding = m_LibraryDesc->spirvBindingOffsets.textureOffset + m_InstanceDesc->resourcesBaseRegisterIndex + sampledBindingIndex++;
        descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      }
      else
      {
        binding =
            m_LibraryDesc->spirvBindingOffsets.storageTextureAndBufferOffset + m_InstanceDesc->resourcesBaseRegisterIndex + storageBindingIndex++;
        descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      }

      writes.push_back(VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = resourceSet,
          .dstBinding      = binding,
          .descriptorCount = 1,
          .descriptorType  = descriptorType,
          .pImageInfo      = &imageInfos.back(),
      });
    }
  }
  vkUpdateDescriptorSets(m_Allocator->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
#endif
}

void PathTraceNrdDenoiser::DispatchNrd(VkCommandBuffer cmd, FrameResources& frameResources, const PathTraceDenoiserResources& denoiserInputs)
{
#ifndef THESIS_ENABLE_NRD
  (void)cmd;
  (void)frameResources;
  (void)denoiserInputs;
  return;
#else
  const nrd::DispatchDesc* dispatchDescs    = nullptr;
  uint32_t                 dispatchDescsNum = 0;
  const nrd::Identifier    denoiserIdentifier = kDenoiserIdentifier;
  assert(IsNrdSuccess(nrd::GetComputeDispatches(*m_Instance, &denoiserIdentifier, 1, dispatchDescs, dispatchDescsNum)));

  for(uint32_t dispatchIndex = 0; dispatchIndex < dispatchDescsNum; ++dispatchIndex)
  {
    const nrd::DispatchDesc& dispatchDesc = dispatchDescs[dispatchIndex];
    VkDescriptorSet          resourceSet  = AllocateDescriptorSet(frameResources, m_ResourceSetLayout);
    UpdateResourceSet(resourceSet, dispatchDesc, denoiserInputs);

    const uint32_t dynamicOffset =
        UploadConstantData(frameResources, dispatchDesc.constantBufferData, dispatchDesc.constantBufferDataSize,
                           dispatchDesc.constantBufferDataMatchesPreviousDispatch);

    const std::array<VkDescriptorSet, 2> descriptorSets{
        resourceSet,
        frameResources.frameSet,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipelines[dispatchDesc.pipelineIndex]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, static_cast<uint32_t>(descriptorSets.size()),
                            descriptorSets.data(), 1, &dynamicOffset);
    vkCmdDispatch(cmd, dispatchDesc.gridWidth, dispatchDesc.gridHeight, 1);
    InsertComputeBarrier(cmd);
  }
#endif
}

void PathTraceNrdDenoiser::ComposeDenoisedResult(VkCommandBuffer cmd,
                                                 FrameResources& frameResources,
                                                 const PathTraceDenoiserResources& denoiserInputs,
                                                 VkImageView rawBeautyImageView,
                                                 VkImageView outputImageView,
                                                 DenoiserDebugView debugView,
                                                 VkExtent2D      viewportSize)
{
  struct ComposePushConstants
  {
    uint32_t debugView = 0;
    uint32_t useMaterialDemodulation = 0;
  };

  const VkDescriptorSet composeSet = AllocateDescriptorSet(frameResources, m_ComposeSetLayout);

  const VkDescriptorImageInfo diffuseImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = m_DiffuseOutputImage.descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo specularImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = m_SpecularOutputImage.descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo rawBeautyImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = rawBeautyImageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo viewZImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetViewZImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo normalRoughnessImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetNormalRoughnessImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo baseColorMetalnessImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetBaseColorMetalnessImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo motionVectorsImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetMotionVectorsImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo noisyDiffuseImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetDiffuseRadianceHitDistanceImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo noisySpecularImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetSpecularRadianceHitDistanceImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorImageInfo outputStorageImageInfo{
      .sampler     = VK_NULL_HANDLE,
      .imageView   = outputImageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const std::array<VkWriteDescriptorSet, 10> writes{{
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &diffuseImageInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 1,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &specularImageInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 2,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &rawBeautyImageInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 3,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &viewZImageInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 4,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &normalRoughnessImageInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 5,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &motionVectorsImageInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 6,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &noisyDiffuseImageInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 7,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &noisySpecularImageInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 8,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &baseColorMetalnessImageInfo,
      },
      VkWriteDescriptorSet{
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = composeSet,
          .dstBinding      = 9,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo      = &outputStorageImageInfo,
      },
  }};
  vkUpdateDescriptorSets(m_Allocator->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

  const ComposePushConstants pushConstants{
      .debugView = static_cast<uint32_t>(debugView),
      .useMaterialDemodulation = m_EnableMaterialDemodulation ? 1u : 0u,
  };

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComposePipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComposePipelineLayout, 0, 1, &composeSet, 0, nullptr);
  vkCmdPushConstants(cmd, m_ComposePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComposePushConstants), &pushConstants);
  vkCmdDispatch(cmd, (viewportSize.width + kComposeGroupSize - 1) / kComposeGroupSize,
                (viewportSize.height + kComposeGroupSize - 1) / kComposeGroupSize, 1);

  InsertComputeBarrier(cmd);
}

const nvvk::Image& PathTraceNrdDenoiser::ResolveDispatchImage(nrd::ResourceType                 resourceType,
                                                              uint16_t                          poolIndex,
                                                              const PathTraceDenoiserResources& denoiserInputs) const
{
#ifndef THESIS_ENABLE_NRD
  (void)resourceType;
  (void)poolIndex;
  return denoiserInputs.GetDiffuseRadianceHitDistanceImage();
#else
  switch(resourceType)
  {
    case nrd::ResourceType::IN_MV:
      return denoiserInputs.GetMotionVectorsImage();
    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
      return denoiserInputs.GetNormalRoughnessImage();
    case nrd::ResourceType::IN_BASECOLOR_METALNESS:
      return denoiserInputs.GetBaseColorMetalnessImage();
    case nrd::ResourceType::IN_VIEWZ:
      return denoiserInputs.GetViewZImage();
    case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
      return denoiserInputs.GetDiffuseRadianceHitDistanceImage();
    case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
      return denoiserInputs.GetSpecularRadianceHitDistanceImage();
    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
      return m_DiffuseOutputImage;
    case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
      return m_SpecularOutputImage;
    case nrd::ResourceType::TRANSIENT_POOL:
      assert(poolIndex < m_TransientPoolImages.size());
      return m_TransientPoolImages[poolIndex];
    case nrd::ResourceType::PERMANENT_POOL:
      assert(poolIndex < m_PermanentPoolImages.size());
      return m_PermanentPoolImages[poolIndex];
    default:
      assert(false && "Unsupported NRD resource type in path-tracer integration");
      return m_DiffuseOutputImage;
  }
#endif
}

void PathTraceNrdDenoiser::TransitionImageToGeneral(VkCommandBuffer cmd, nvvk::Image& image, VkPipelineStageFlags2 dstStageMask) const
{
  if(image.image == VK_NULL_HANDLE || image.descriptor.imageLayout == VK_IMAGE_LAYOUT_GENERAL)
  {
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

void PathTraceNrdDenoiser::InsertComputeBarrier(VkCommandBuffer cmd) const
{
  const VkMemoryBarrier2 memoryBarrier{
      .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
  };
  const VkDependencyInfo dependencyInfo{
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &memoryBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

#ifdef THESIS_ENABLE_NRD
VkDescriptorType PathTraceNrdDenoiser::ToVkDescriptorType(nrd::DescriptorType descriptorType)
{
  switch(descriptorType)
  {
    case nrd::DescriptorType::TEXTURE:
      return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case nrd::DescriptorType::STORAGE_TEXTURE:
      return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    default:
      assert(false && "Unsupported NRD descriptor type");
      return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  }
}

VkFormat PathTraceNrdDenoiser::ToVkFormat(nrd::Format format)
{
  switch(format)
  {
    case nrd::Format::R8_UNORM:
      return VK_FORMAT_R8_UNORM;
    case nrd::Format::R8_SNORM:
      return VK_FORMAT_R8_SNORM;
    case nrd::Format::R8_UINT:
      return VK_FORMAT_R8_UINT;
    case nrd::Format::R8_SINT:
      return VK_FORMAT_R8_SINT;
    case nrd::Format::RG8_UNORM:
      return VK_FORMAT_R8G8_UNORM;
    case nrd::Format::RG8_SNORM:
      return VK_FORMAT_R8G8_SNORM;
    case nrd::Format::RG8_UINT:
      return VK_FORMAT_R8G8_UINT;
    case nrd::Format::RG8_SINT:
      return VK_FORMAT_R8G8_SINT;
    case nrd::Format::RGBA8_UNORM:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM:
      return VK_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::RGBA8_UINT:
      return VK_FORMAT_R8G8B8A8_UINT;
    case nrd::Format::RGBA8_SINT:
      return VK_FORMAT_R8G8B8A8_SINT;
    case nrd::Format::RGBA8_SRGB:
      return VK_FORMAT_R8G8B8A8_SRGB;
    case nrd::Format::R16_UNORM:
      return VK_FORMAT_R16_UNORM;
    case nrd::Format::R16_SNORM:
      return VK_FORMAT_R16_SNORM;
    case nrd::Format::R16_UINT:
      return VK_FORMAT_R16_UINT;
    case nrd::Format::R16_SINT:
      return VK_FORMAT_R16_SINT;
    case nrd::Format::R16_SFLOAT:
      return VK_FORMAT_R16_SFLOAT;
    case nrd::Format::RG16_UNORM:
      return VK_FORMAT_R16G16_UNORM;
    case nrd::Format::RG16_SNORM:
      return VK_FORMAT_R16G16_SNORM;
    case nrd::Format::RG16_UINT:
      return VK_FORMAT_R16G16_UINT;
    case nrd::Format::RG16_SINT:
      return VK_FORMAT_R16G16_SINT;
    case nrd::Format::RG16_SFLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case nrd::Format::RGBA16_UNORM:
      return VK_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_UINT:
      return VK_FORMAT_R16G16B16A16_UINT;
    case nrd::Format::RGBA16_SINT:
      return VK_FORMAT_R16G16B16A16_SINT;
    case nrd::Format::RGBA16_SFLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case nrd::Format::R32_UINT:
      return VK_FORMAT_R32_UINT;
    case nrd::Format::R32_SINT:
      return VK_FORMAT_R32_SINT;
    case nrd::Format::R32_SFLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case nrd::Format::RG32_UINT:
      return VK_FORMAT_R32G32_UINT;
    case nrd::Format::RG32_SINT:
      return VK_FORMAT_R32G32_SINT;
    case nrd::Format::RG32_SFLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    case nrd::Format::RGB32_UINT:
      return VK_FORMAT_R32G32B32_UINT;
    case nrd::Format::RGB32_SINT:
      return VK_FORMAT_R32G32B32_SINT;
    case nrd::Format::RGB32_SFLOAT:
      return VK_FORMAT_R32G32B32_SFLOAT;
    case nrd::Format::RGBA32_UINT:
      return VK_FORMAT_R32G32B32A32_UINT;
    case nrd::Format::RGBA32_SINT:
      return VK_FORMAT_R32G32B32A32_SINT;
    case nrd::Format::RGBA32_SFLOAT:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case nrd::Format::R10_G10_B10_A2_UINT:
      return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case nrd::Format::R11_G11_B10_UFLOAT:
      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:
      return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default:
      assert(false && "Unsupported NRD format");
      return VK_FORMAT_UNDEFINED;
  }
}

void PathTraceNrdDenoiser::CopyMatrix(glm::mat4 matrix, float (&destination)[16])
{
  std::memcpy(destination, glm::value_ptr(matrix), sizeof(destination));
}
#endif

}  // namespace nvsamples
