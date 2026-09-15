#include "NrdDenoiser.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/gtc/type_ptr.hpp>
#include "Framework/Vulkan/Diagnostics.h"

namespace rtpt
{

namespace
{

// Denoised outputs are half float, matching the renderer's noisy radiance inputs.
constexpr VkFormat kDenoisedRadianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

template <typename T>
T AlignTo(T value, T alignment)
{
  return (value + alignment - 1) / alignment * alignment;
}

uint32_t DivideUp(uint32_t x, uint16_t y)
{
  return (x + y - 1u) / y;
}

bool IsNrdSuccess(nrd::Result result)
{
  return result == nrd::Result::SUCCESS;
}

}  // namespace

NrdDenoiser::NrdDenoiser(const CreateInfo& createInfo)
    : m_Device(createInfo.device)
    , m_Resources(createInfo.resources)
    , m_Diagnostics(createInfo.diagnostics)
    , m_FrameSlotCount(createInfo.frameSlotCount)
    , m_ComposePass(NrdComposePass::CreateInfo { .device = createInfo.device, .frameSlotCount = createInfo.frameSlotCount })
{
}

void NrdDenoiser::Initialize()
{
  // Missing dependencies or a repeat call leave the denoiser not ready rather than failing; callers check IsReady.
  if(m_Device == VK_NULL_HANDLE || m_Resources == nullptr || m_FrameSlotCount == 0 || m_Instance != nullptr)
  {
    return;
  }

  m_LibraryDesc = nrd::GetLibraryDesc();

  // NRD instance
  // One REBLUR_DIFFUSE_SPECULAR denoiser matches the renderers' split diffuse/specular signals.

  const nrd::DenoiserDesc denoiserDesc {
      .identifier = kDenoiserIdentifier,
      .denoiser   = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR,
  };

  const nrd::InstanceCreationDesc instanceCreateInfo {
      .denoisers   = &denoiserDesc,
      .denoisersNum = 1,
  };

  if(!IsNrdSuccess(nrd::CreateInstance(instanceCreateInfo, m_Instance)) || m_Instance == nullptr)
  {
    m_Instance = nullptr;
    return;
  }

  m_InstanceDesc = nrd::GetInstanceDesc(*m_Instance);

  // Constant buffer alignment
  // Each dispatch's constants live at a dynamic offset into one uniform buffer, so offsets must respect the device's minimum alignment.

  VkPhysicalDeviceProperties2 properties {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
  };

  vkGetPhysicalDeviceProperties2(m_Resources->PhysicalDevice(), &properties);

  m_ConstantBufferAlignment = std::max(1u, static_cast<uint32_t>(properties.properties.limits.minUniformBufferOffsetAlignment));

  // Defaults until a renderer supplies DenoiserSettings; the values match the DenoiserSettings defaults.
  m_ReblurSettings.maxAccumulatedFrameNum      = 30;
  m_ReblurSettings.maxFastAccumulatedFrameNum  = 6;

  CreateVulkanState();
}

void NrdDenoiser::Destroy()
{
  if(m_Resources == nullptr)
  {
    return;
  }

  // Vulkan objects go first; they were built from m_InstanceDesc, which the NRD instance owns.
  DestroyVulkanState();

  if(m_Instance != nullptr)
  {
    nrd::DestroyInstance(*m_Instance);
  }

  // Reset to the pre-Initialize state so a later Initialize starts from a clean history.

  m_LibraryDesc            = nullptr;
  m_InstanceDesc           = nullptr;
  m_Instance               = nullptr;
  m_HasPreviousMatrices    = false;
  m_PreviousViewMatrix     = glm::mat4(1.0f);
  m_PreviousProjectionMatrix = glm::mat4(1.0f);
  m_FrameIndex             = 0;
  m_CurrentFrameSlot       = 0;
}

bool NrdDenoiser::IsReady() const
{
  return m_Instance != nullptr && m_PipelineLayout != VK_NULL_HANDLE && !m_Pipelines.empty() && m_ComposePass.IsReady();
}

void NrdDenoiser::InvalidateHistory()
{
  // The next PrepareFrame requests CLEAR_AND_RESTART and treats the camera as having no previous frame.

  m_HistoryInvalidated   = true;
  m_HasPreviousMatrices  = false;
  m_FrameIndex           = 0;
}

void NrdDenoiser::PrepareFrame(const FrameInput& input, const DenoiserResources& denoiserInputs)
{
  if(!IsReady() || input.sceneInfo == nullptr || input.viewportSize.width == 0 || input.viewportSize.height == 0)
  {
    return;
  }

  // Frame state
  // The renderer's inputs are not read here; they are bound in Denoise.
  // A resize inside EnsureForViewport also invalidates history, so it runs before the invalidation is resolved below.

  (void)denoiserInputs;

  EnsureForViewport(input.viewportSize);

  m_EnableMaterialDemodulation = input.enableMaterialDemodulation;
  m_CurrentFrameSlot = std::min(input.frameSlot, m_FrameSlotCount - 1);

  if(input.settings != nullptr)
  {
    ApplyDenoiserSettings(*input.settings);
  }

  m_HistoryInvalidated = m_HistoryInvalidated || input.historyInvalidated;

  // NRD's frame index sequence restarts together with the history.
  if(m_HistoryInvalidated)
  {
    m_FrameIndex = 0;
  }

  UpdateCommonSettings(input);

  // Submit settings
  // Kept out of assert(): NDEBUG removes the whole expression, so wrapping these would mean NRD never receives camera matrices, frame index or REBLUR settings in a release build.
  // And it fails silently, because a denoiser with no settings still dispatches; it just denoises against garbage.

  const nrd::Result commonSettingsResult = nrd::SetCommonSettings(*m_Instance, m_CommonSettings);
  assert(IsNrdSuccess(commonSettingsResult));

  const nrd::Result denoiserSettingsResult = nrd::SetDenoiserSettings(*m_Instance, kDenoiserIdentifier, &m_ReblurSettings);
  assert(IsNrdSuccess(denoiserSettingsResult));

  if(!IsNrdSuccess(commonSettingsResult) || !IsNrdSuccess(denoiserSettingsResult))
  {
    // Leave the invalidation pending so the next frame retries from a clean history rather than accumulating onto settings NRD rejected.
    return;
  }

  m_HistoryInvalidated = false;
}

void NrdDenoiser::ApplyDenoiserSettings(const DenoiserSettings& settings)
{
  // Accumulation and blur
  // The fast history length is clamped to the main one.

  m_ReblurSettings.maxAccumulatedFrameNum      = settings.maxAccumulatedFrames;
  m_ReblurSettings.maxFastAccumulatedFrameNum  = std::min(settings.maxFastAccumulatedFrames, settings.maxAccumulatedFrames);
  m_ReblurSettings.diffusePrepassBlurRadius    = settings.diffusePrepassBlurRadius;
  m_ReblurSettings.specularPrepassBlurRadius   = settings.specularPrepassBlurRadius;
  m_ReblurSettings.enableAntiFirefly           = settings.enableAntiFirefly;
  m_ReblurSettings.maxBlurRadius               = settings.maxBlurRadius;

  // Hit distance normalization
  // Same three values the shaders normalize with; see DenoiserSettings.

  m_ReblurSettings.hitDistanceParameters.A     = settings.hitDistanceA;
  m_ReblurSettings.hitDistanceParameters.B     = settings.hitDistanceB;
  m_ReblurSettings.hitDistanceParameters.C     = settings.hitDistanceC;

  // Hit distance reconstruction
  // Per renderer, not global: only a renderer that leaves one lobe's hit distance at zero wants NRD to go looking for a replacement.

  switch(settings.hitDistanceReconstructionMode)
  {
    case HitDistanceReconstructionMode::eArea3x3:
      m_ReblurSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
      break;
    case HitDistanceReconstructionMode::eArea5x5:
      m_ReblurSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_5X5;
      break;
    case HitDistanceReconstructionMode::eOff:
    default:
      m_ReblurSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
      break;
  }
}

void NrdDenoiser::Denoise(VkCommandBuffer cmd, const DenoiserResources& denoiserInputs, VkImageView rawBeautyImageView, VkImageView outputImageView, DenoiserDebugView debugView, VkExtent2D viewportSize)
{
  if(!IsReady() || cmd == VK_NULL_HANDLE || rawBeautyImageView == VK_NULL_HANDLE || outputImageView == VK_NULL_HANDLE || viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  EnsureForViewport(viewportSize);

  // Image layouts
  // NRD binds every image with GENERAL layout. The renderer's inputs are normally GENERAL already, since it wrote them as storage images; the transition only acts on images still tracked in another layout.
  // The const_casts are needed because the transition records the new layout on the image, while DenoiserResources is only passed here as const.

  TransitionImageToGeneral(cmd, const_cast<rtpt::Image&>(denoiserInputs.GetMotionVectorsImage()), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<rtpt::Image&>(denoiserInputs.GetNormalRoughnessImage()), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<rtpt::Image&>(denoiserInputs.GetBaseColorMetalnessImage()), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<rtpt::Image&>(denoiserInputs.GetViewZImage()), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<rtpt::Image&>(denoiserInputs.GetDiffuseRadianceHitDistanceImage()), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, const_cast<rtpt::Image&>(denoiserInputs.GetSpecularRadianceHitDistanceImage()), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, m_DiffuseOutputImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  TransitionImageToGeneral(cmd, m_SpecularOutputImage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

  for(rtpt::Image& image : m_PermanentPoolImages)
  {
    TransitionImageToGeneral(cmd, image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  }

  for(rtpt::Image& image : m_TransientPoolImages)
  {
    TransitionImageToGeneral(cmd, image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  }

  // Record
  // BeginFrame resets this slot's pool and constant offsets, so it must precede every descriptor allocation and upload that follows.

  FrameResources& frameResources = GetCurrentFrameResources();

  BeginFrame(frameResources);
  UpdateFrameSet(frameResources);
  DispatchNrd(cmd, frameResources, denoiserInputs);

  // Compose
  // The last NRD dispatch already published its writes, so the compose pass reads the outputs directly and only its own result needs a barrier after.

  const NrdComposePass::RecordInput composeInput {
      .denoiserInputs             = &denoiserInputs,
      .denoisedDiffuseImageView   = m_DiffuseOutputImage.descriptor.imageView,
      .denoisedSpecularImageView  = m_SpecularOutputImage.descriptor.imageView,
      .rawBeautyImageView         = rawBeautyImageView,
      .outputImageView            = outputImageView,
      .debugView                  = debugView,
      .enableMaterialDemodulation = m_EnableMaterialDemodulation,
      .viewportSize               = viewportSize,
      .frameSlot                  = m_CurrentFrameSlot,
  };

  m_ComposePass.Record(cmd, composeInput);

  // Makes the composed output visible to whatever compute work the renderer records next.
  InsertComputeBarrier(cmd);
}

const rtpt::Image& NrdDenoiser::GetDiffuseOutputImage() const
{
  return m_DiffuseOutputImage;
}

const rtpt::Image& NrdDenoiser::GetSpecularOutputImage() const
{
  return m_SpecularOutputImage;
}

void NrdDenoiser::CreateVulkanState()
{
  // Order follows dependencies: the frame layout embeds the samplers, the pipeline layout uses the set layouts, and the pipelines use the pipeline layout.
  // The compose pass is independent of NRD's objects; it is built after NRD's pipelines so the creation order matches the dispatch order.

  CreateSamplers();
  CreateDescriptorSetLayouts();
  CreatePipelineLayout();
  CreatePipelines();
  m_ComposePass.Initialize();
  CreateFrameResources();
}

void NrdDenoiser::DestroyVulkanState()
{
  if(m_Resources == nullptr)
  {
    return;
  }

  VkDevice device = m_Device;

  // Images and per-slot pools go before the layouts and pipelines they were created against.

  DestroyViewportResources();
  DestroyFrameResources();

  for(VkPipeline pipeline : m_Pipelines)
  {
    vkDestroyPipeline(device, pipeline, nullptr);
  }

  m_Pipelines.clear();

  // The compose pass owns its pipeline, layouts, and sets, and releases them in the same place the compose pipeline used to go.
  m_ComposePass.Destroy();

  // Destroying VK_NULL_HANDLE is a no-op, so this is safe after a partial or failed Initialize.

  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
  vkDestroyDescriptorSetLayout(device, m_ResourceSetLayout, nullptr);
  vkDestroyDescriptorSetLayout(device, m_FrameSetLayout, nullptr);
  vkDestroySampler(device, m_NearestSampler, nullptr);
  vkDestroySampler(device, m_LinearSampler, nullptr);

  // Nulled handles make IsReady false and a repeated Destroy harmless.

  m_PipelineLayout    = VK_NULL_HANDLE;
  m_ResourceSetLayout = VK_NULL_HANDLE;
  m_FrameSetLayout    = VK_NULL_HANDLE;
  m_NearestSampler    = VK_NULL_HANDLE;
  m_LinearSampler     = VK_NULL_HANDLE;
}

void NrdDenoiser::CreateSamplers()
{
  // Samplers
  // The two samplers NRD's shaders declare, nrd::Sampler::NEAREST_CLAMP and LINEAR_CLAMP, in that order.

  VkDevice device = m_Device;

  const VkSamplerCreateInfo nearestSamplerInfo {
      .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter    = VK_FILTER_NEAREST,
      .minFilter    = VK_FILTER_NEAREST,
      .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod       = VK_LOD_CLAMP_NONE,
  };

  rtpt::CheckVk(vkCreateSampler(device, &nearestSamplerInfo, nullptr, &m_NearestSampler), "vkCreateSampler(NRD nearest)");

  const VkSamplerCreateInfo linearSamplerInfo {
      .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter    = VK_FILTER_LINEAR,
      .minFilter    = VK_FILTER_LINEAR,
      .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod       = VK_LOD_CLAMP_NONE,
  };

  rtpt::CheckVk(vkCreateSampler(device, &linearSamplerInfo, nullptr, &m_LinearSampler), "vkCreateSampler(NRD linear)");
}

void NrdDenoiser::CreateDescriptorSetLayouts()
{
  VkDevice device = m_Device;

  // Binding numbers
  // NRD describes the SPIR-V binding ranges for its generated compute passes.
  // The wrapper mirrors that layout in Vulkan instead of hard-coding a local descriptor contract.

  const uint32_t textureBinding = m_LibraryDesc->spirvBindingOffsets.textureOffset + m_InstanceDesc->resourcesBaseRegisterIndex;
  const uint32_t storageBinding = m_LibraryDesc->spirvBindingOffsets.storageTextureAndBufferOffset + m_InstanceDesc->resourcesBaseRegisterIndex;
  const uint32_t samplerBinding = m_LibraryDesc->spirvBindingOffsets.samplerOffset + m_InstanceDesc->samplersBaseRegisterIndex;
  const uint32_t constantBufferBinding = m_LibraryDesc->spirvBindingOffsets.constantBufferOffset + m_InstanceDesc->constantBufferRegisterIndex;

  // Resource set
  // Sized for the largest dispatch: perSetTexturesMaxNum sampled images followed by perSetStorageTexturesMaxNum storage images.
  // A dispatch that uses fewer leaves the remaining bindings unwritten, which is valid as long as the dispatch's pipeline does not statically use them.

  std::vector<VkDescriptorSetLayoutBinding> resourceBindings;
  std::vector<VkDescriptorBindingFlags>     resourceBindingFlags;

  resourceBindings.reserve(m_InstanceDesc->descriptorPoolDesc.perSetTexturesMaxNum + m_InstanceDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum);
  resourceBindingFlags.reserve(resourceBindings.capacity());

  for(uint32_t textureIndex = 0; textureIndex < m_InstanceDesc->descriptorPoolDesc.perSetTexturesMaxNum; ++textureIndex)
  {
    resourceBindings.push_back(VkDescriptorSetLayoutBinding {
        .binding         = textureBinding + textureIndex,
        .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
    });
    resourceBindingFlags.push_back(0);
  }

  for(uint32_t storageIndex = 0; storageIndex < m_InstanceDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum; ++storageIndex)
  {
    resourceBindings.push_back(VkDescriptorSetLayoutBinding {
        .binding         = storageBinding + storageIndex,
        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
    });
    resourceBindingFlags.push_back(0);
  }

  const VkDescriptorSetLayoutBindingFlagsCreateInfo resourceBindingFlagsInfo {
      .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
      .bindingCount  = static_cast<uint32_t>(resourceBindingFlags.size()),
      .pBindingFlags = resourceBindingFlags.data(),
  };

  const VkDescriptorSetLayoutCreateInfo resourceLayoutInfo {
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext        = &resourceBindingFlagsInfo,
      .bindingCount = static_cast<uint32_t>(resourceBindings.size()),
      .pBindings    = resourceBindings.data(),
  };

  rtpt::CheckVk(vkCreateDescriptorSetLayout(device, &resourceLayoutInfo, nullptr, &m_ResourceSetLayout), "vkCreateDescriptorSetLayout(NRD resources)");

  // Frame set
  // Samplers are immutable, baked into the layout in nrd::Sampler order, so the set only ever needs its constant buffer written.
  // The constant buffer is dynamic so one set serves every dispatch, each at its own offset.

  const VkSampler samplers[] = { m_NearestSampler, m_LinearSampler };
  assert(m_InstanceDesc->samplersNum <= std::size(samplers));

  std::vector<VkDescriptorSetLayoutBinding> frameBindings;
  frameBindings.reserve(m_InstanceDesc->samplersNum + 1);

  for(uint32_t samplerIndex = 0; samplerIndex < m_InstanceDesc->samplersNum; ++samplerIndex)
  {
    frameBindings.push_back(VkDescriptorSetLayoutBinding {
        .binding            = samplerBinding + samplerIndex,
        .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
        .descriptorCount    = 1,
        .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = &samplers[samplerIndex],
    });
  }

  frameBindings.push_back(VkDescriptorSetLayoutBinding {
      .binding         = constantBufferBinding,
      .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
      .descriptorCount = 1,
      .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
  });

  const VkDescriptorSetLayoutCreateInfo frameLayoutInfo {
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(frameBindings.size()),
      .pBindings    = frameBindings.data(),
  };

  rtpt::CheckVk(vkCreateDescriptorSetLayout(device, &frameLayoutInfo, nullptr, &m_FrameSetLayout), "vkCreateDescriptorSetLayout(NRD frame)");
}

void NrdDenoiser::CreatePipelineLayout()
{
  VkDevice device = m_Device;

  // NRD pipeline layout
  // NRD's shaders declare their per-dispatch resources in register space resourcesSpaceIndex and their samplers and constant buffer in constantBufferAndSamplersSpaceIndex, and DXC maps register space N to descriptor set N.
  // This layout and the binding in DispatchNrd put the resource set at set 0 and the frame set at set 1, so the instance must report exactly those spaces; any other NRD build would bind every descriptor to the wrong set.

  const uint32_t resourcesSpace = m_InstanceDesc->resourcesSpaceIndex;
  const uint32_t frameSpace     = m_InstanceDesc->constantBufferAndSamplersSpaceIndex;

  if(resourcesSpace != 0 || frameSpace != 1)
  {
    throw std::runtime_error("NRD register spaces do not match the pipeline layout: expected resources in space 0 and constants and samplers in space 1, got " + std::to_string(resourcesSpace) + " and " + std::to_string(frameSpace));
  }

  const std::array<VkDescriptorSetLayout, 2> setLayouts {
      m_ResourceSetLayout,
      m_FrameSetLayout,
  };

  const VkPipelineLayoutCreateInfo pipelineLayoutInfo {
      .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
      .pSetLayouts    = setLayouts.data(),
  };

  rtpt::CheckVk(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout), "vkCreatePipelineLayout(NRD)");
}

void NrdDenoiser::CreatePipelines()
{
  VkDevice device = m_Device;

  // NRD pipelines
  // One compute pipeline per NRD pipeline description, pushed in order so DispatchDesc::pipelineIndex indexes m_Pipelines directly.
  // Shader modules are only needed until pipeline creation, so each is destroyed right after.

  m_Pipelines.reserve(m_InstanceDesc->pipelinesNum);

  for(uint32_t pipelineIndex = 0; pipelineIndex < m_InstanceDesc->pipelinesNum; ++pipelineIndex)
  {
    const nrd::PipelineDesc& pipelineDesc = m_InstanceDesc->pipelines[pipelineIndex];

    const VkShaderModuleCreateInfo shaderCode {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = static_cast<size_t>(pipelineDesc.computeShaderSPIRV.size),
        .pCode    = static_cast<const uint32_t*>(pipelineDesc.computeShaderSPIRV.bytecode),
    };

    VkShaderModule shaderModule = VK_NULL_HANDLE;

    rtpt::CheckVk(vkCreateShaderModule(device, &shaderCode, nullptr, &shaderModule), "vkCreateShaderModule(NRD)");

    const VkPipelineShaderStageCreateInfo shaderStage {
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shaderModule,
        .pName  = m_InstanceDesc->shaderEntryPoint,
    };

    const VkComputePipelineCreateInfo pipelineInfo {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = shaderStage,
        .layout = m_PipelineLayout,
    };

    VkPipeline pipeline = VK_NULL_HANDLE;

    rtpt::CheckVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines(NRD)");

    m_Pipelines.push_back(pipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
  }
}

void NrdDenoiser::CreateFrameResources()
{
  const uint32_t frameCount = m_FrameSlotCount;

  m_FrameResources.resize(frameCount);

  for(FrameResources& frameResources : m_FrameResources)
  {
    // Descriptor pool
    // Sized for NRD's worst case of setsMaxNum resource sets, plus the frame set (the + 1 on maxSets).
    // The frame set adds the single dynamic uniform buffer and its immutable sampler bindings. The spec does not say whether immutable samplers consume pool space, so they are reserved to stay safe. The compose set comes from NrdComposePass's own pool.

    const VkDescriptorPoolSize poolSizes[] = {
        VkDescriptorPoolSize {
            .type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = m_InstanceDesc->descriptorPoolDesc.setsMaxNum * m_InstanceDesc->descriptorPoolDesc.perSetTexturesMaxNum,
        },
        VkDescriptorPoolSize {
            .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = m_InstanceDesc->descriptorPoolDesc.setsMaxNum * m_InstanceDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum,
        },
        VkDescriptorPoolSize {
            .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .descriptorCount = 1,
        },
        VkDescriptorPoolSize {
            .type            = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = m_InstanceDesc->samplersNum,
        },
    };

    const VkDescriptorPoolCreateInfo poolInfo {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = m_InstanceDesc->descriptorPoolDesc.setsMaxNum + 1,
        .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
        .pPoolSizes    = poolSizes,
    };

    rtpt::CheckVk(vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &frameResources.descriptorPool), "vkCreateDescriptorPool(NRD frame)");

    // Constant buffer
    // Room for one maximum-size, aligned constant block per possible dispatch, host-mapped so uploads are a memcpy and a flush.

    const uint32_t constantBufferSize = std::max(1u, m_InstanceDesc->descriptorPoolDesc.setsMaxNum) * AlignTo(m_InstanceDesc->constantBufferMaxDataSize, m_ConstantBufferAlignment);

    rtpt::CheckVk(m_Resources->CreateBuffer(frameResources.constantBuffer, constantBufferSize, VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT), "ResourceAllocator::CreateBuffer(NRD constants)");
  }
}

void NrdDenoiser::DestroyFrameResources()
{
  if(m_Resources == nullptr)
  {
    return;
  }

  VkDevice device = m_Device;

  // Destroying a pool frees every set allocated from it, so the sets need no separate release.
  for(FrameResources& frameResources : m_FrameResources)
  {
    frameResources.constantBuffer.Reset();
    vkDestroyDescriptorPool(device, frameResources.descriptorPool, nullptr);
    frameResources = {};
  }

  m_FrameResources.clear();
}

void NrdDenoiser::DestroyViewportResources()
{
  m_DiffuseOutputImage.Reset();
  m_SpecularOutputImage.Reset();
  m_PermanentPoolImages.clear();
  m_TransientPoolImages.clear();

  // Clearing the size forces the next EnsureForViewport to reallocate.
  m_ViewportSize = {};
}

void NrdDenoiser::EnsureForViewport(VkExtent2D viewportSize)
{
  // A zero extent keeps the current images rather than allocating nothing.
  if(viewportSize.width == 0 || viewportSize.height == 0)
  {
    return;
  }

  // Reallocating at an unchanged size would throw away NRD's history.
  if(m_ViewportSize.width == viewportSize.width && m_ViewportSize.height == viewportSize.height && m_DiffuseOutputImage.image != VK_NULL_HANDLE)
  {
    return;
  }

  RecreateViewportResources(viewportSize);
}

void NrdDenoiser::RecreateViewportResources(VkExtent2D viewportSize)
{
  DestroyViewportResources();

  // The new pool images hold no history, so NRD must clear and restart on the next frame.

  m_ViewportSize       = viewportSize;
  m_HistoryInvalidated = true;

  // Outputs

  m_DiffuseOutputImage  = CreateStorageImage(viewportSize, kDenoisedRadianceFormat, "NrdDiffuseOutput");
  m_SpecularOutputImage = CreateStorageImage(viewportSize, kDenoisedRadianceFormat, "NrdSpecularOutput");

  // Internal pools
  // NRD describes each pool image by format and a downsample factor relative to the viewport; the vectors keep NRD's order so indexInPool maps directly.

  m_PermanentPoolImages.reserve(m_InstanceDesc->permanentPoolSize);

  for(uint32_t imageIndex = 0; imageIndex < m_InstanceDesc->permanentPoolSize; ++imageIndex)
  {
    const VkExtent2D imageSize {
        .width  = DivideUp(viewportSize.width, m_InstanceDesc->permanentPool[imageIndex].downsampleFactor),
        .height = DivideUp(viewportSize.height, m_InstanceDesc->permanentPool[imageIndex].downsampleFactor),
    };

    m_PermanentPoolImages.push_back(CreateStorageImage(imageSize, ToVkFormat(m_InstanceDesc->permanentPool[imageIndex].format), "NrdPermanentPool"));
  }

  m_TransientPoolImages.reserve(m_InstanceDesc->transientPoolSize);

  for(uint32_t imageIndex = 0; imageIndex < m_InstanceDesc->transientPoolSize; ++imageIndex)
  {
    const VkExtent2D imageSize {
        .width  = DivideUp(viewportSize.width, m_InstanceDesc->transientPool[imageIndex].downsampleFactor),
        .height = DivideUp(viewportSize.height, m_InstanceDesc->transientPool[imageIndex].downsampleFactor),
    };

    m_TransientPoolImages.push_back(CreateStorageImage(imageSize, ToVkFormat(m_InstanceDesc->transientPool[imageIndex].format), "NrdTransientPool"));
  }
}

rtpt::Image NrdDenoiser::CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const
{
  rtpt::Image image;

  // Image description
  // Every NRD image is both read (sampled) and written (storage) by some pass.

  VkImageCreateInfo imageInfo {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = format,
      .extent        = { .width = viewportSize.width, .height = viewportSize.height, .depth = 1 },
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  VkImageViewCreateInfo viewInfo {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = format,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
  };

  // Allocate
  // descriptor.imageLayout is the CPU-side layout record TransitionImageToGeneral reads, so it starts as UNDEFINED. Samplers come from the frame set, never the image descriptor.

  rtpt::CheckVk(m_Resources->CreateImage(image, imageInfo, &viewInfo), "ResourceAllocator::CreateImage(NRD target)");

  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image.descriptor.sampler     = VK_NULL_HANDLE;

  if(m_Diagnostics != nullptr)
  {
    m_Diagnostics->SetObjectName(m_Device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image.image), debugName);
  }

  return image;
}

void NrdDenoiser::UpdateCommonSettings(const FrameInput& input)
{
  const shaderio::GltfSceneInfo& sceneInfo = *input.sceneInfo;

  // Camera matrices
  // NRD receives current and previous camera transforms in view/clip space.
  // The projection is recovered as viewProj * viewInverse because the scene info carries no standalone projection matrix.
  // With no previous frame since a reset, the current matrices stand in for the previous ones so NRD sees a still camera.

  const glm::mat4 currentViewMatrix       = sceneInfo.viewMatrix;
  const glm::mat4 currentProjectionMatrix = sceneInfo.viewProjMatrix * sceneInfo.viewInvMatrix;
  const glm::mat4 previousViewMatrix      = m_HasPreviousMatrices ? m_PreviousViewMatrix : currentViewMatrix;
  const glm::mat4 previousProjectionMatrix = m_HasPreviousMatrices ? m_PreviousProjectionMatrix : currentProjectionMatrix;

  CopyMatrix(currentProjectionMatrix, m_CommonSettings.viewToClipMatrix);
  CopyMatrix(previousProjectionMatrix, m_CommonSettings.viewToClipMatrixPrev);
  CopyMatrix(currentViewMatrix, m_CommonSettings.worldToViewMatrix);
  CopyMatrix(previousViewMatrix, m_CommonSettings.worldToViewMatrixPrev);

  // Motion, jitter, and resolution
  // The renderer has no jitter here, and motion vectors are written as screen-UV deltas with Z as previous-viewZ minus current-viewZ.
  // The z scale is 1 rather than NRD's 2D default of 0, so NRD uses that viewZ delta. Resource and rect sizes both equal the viewport because there is no dynamic resolution.

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

  // Depth, history, and optional inputs
  // viewZ is written in full float, so no scale is needed. Sky pixels carry a 1e32 sentinel, far beyond denoisingRange, so NRD ignores them.
  // Base colour / metalness is no longer an NRD input: 4.17 dropped IN_BASECOLOR_METALNESS, which earlier versions used only to patch motion vectors where specular motion prevailed. The image itself is still produced, because the compose pass needs it to rebuild the diffuse demodulation factor.

  m_CommonSettings.viewZScale           = 1.0f;
  m_CommonSettings.denoisingRange       = 500000.0f;

  // This is NRD's local history rejection threshold. Higher values keep more reprojected history through camera motion, but can also make trails easier to see.
  m_CommonSettings.disocclusionThreshold = input.settings != nullptr ? input.settings->disocclusionThreshold : DenoiserSettings {}.disocclusionThreshold;

  m_CommonSettings.disocclusionThresholdAlternate = 0.05f;
  m_CommonSettings.splitScreen          = 0.0f;
  m_CommonSettings.frameIndex           = m_FrameIndex;
  m_CommonSettings.accumulationMode = m_HistoryInvalidated ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
  m_CommonSettings.isMotionVectorInWorldSpace        = false;
  m_CommonSettings.isHistoryConfidenceAvailable      = false;
  m_CommonSettings.isDisocclusionThresholdMixAvailable = false;
  m_CommonSettings.enableValidation                  = false;

  // NRD turns this into a frame-rate scale, max(33.3 ms / delta, 1), that REBLUR's temporal accumulation and anti-lag use to decide how strongly history is kept.
  m_CommonSettings.timeDeltaBetweenFrames = input.frameTimeMilliseconds;

  // Advance history
  // The current matrices become next frame's previous matrices, and NRD's frame index advances by exactly one per frame.

  m_PreviousViewMatrix       = currentViewMatrix;
  m_PreviousProjectionMatrix = currentProjectionMatrix;
  m_HasPreviousMatrices      = true;
  m_FrameIndex += 1;
}

NrdDenoiser::FrameResources& NrdDenoiser::GetCurrentFrameResources()
{
  assert(!m_FrameResources.empty());

  const uint32_t frameIndex = std::min(m_CurrentFrameSlot, uint32_t(m_FrameResources.size() - 1));

  return m_FrameResources[frameIndex];
}

void NrdDenoiser::BeginFrame(FrameResources& frameResources)
{
  // Reset slot state
  // This slot's previous frame has finished by the time the slot records again, so its sets and constant offsets can be reused wholesale.

  frameResources.constantBufferOffset        = 0;
  frameResources.previousConstantBufferOffset = 0;
  frameResources.frameSet                    = VK_NULL_HANDLE;

  rtpt::CheckVk(vkResetDescriptorPool(m_Device, frameResources.descriptorPool, 0), "vkResetDescriptorPool(NRD frame)");

  // The frame set is allocated first so every dispatch can bind it.
  frameResources.frameSet = AllocateDescriptorSet(frameResources, m_FrameSetLayout);
}

void NrdDenoiser::UpdateFrameSet(FrameResources& frameResources)
{
  // Constant buffer binding
  // Written once at offset 0 with the maximum constant size; each dispatch supplies its real offset as the dynamic offset.

  const VkDescriptorBufferInfo constantBufferInfo {
      .buffer = frameResources.constantBuffer.buffer,
      .offset = 0,
      .range  = m_InstanceDesc->constantBufferMaxDataSize,
  };

  const VkWriteDescriptorSet write {
      .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet          = frameResources.frameSet,
      .dstBinding      = m_LibraryDesc->spirvBindingOffsets.constantBufferOffset + m_InstanceDesc->constantBufferRegisterIndex,
      .descriptorCount = 1,
      .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
      .pBufferInfo     = &constantBufferInfo,
  };

  vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

VkDescriptorSet NrdDenoiser::AllocateDescriptorSet(FrameResources& frameResources, VkDescriptorSetLayout layout)
{
  const VkDescriptorSetAllocateInfo allocInfo {
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = frameResources.descriptorPool,
      .descriptorSetCount = 1,
      .pSetLayouts        = &layout,
  };

  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

  rtpt::CheckVk(vkAllocateDescriptorSets(m_Device, &allocInfo, &descriptorSet), "vkAllocateDescriptorSets(NRD)");

  return descriptorSet;
}

uint32_t NrdDenoiser::UploadConstantData(FrameResources& frameResources, const void* constantData, uint32_t constantDataSize, bool reusePreviousData)
{
  // A dispatch with no constants still needs a valid dynamic offset; 0 is always in range.
  if(constantDataSize == 0 || constantData == nullptr)
  {
    return 0;
  }

  // NRD flags constants identical to the previous dispatch, so that upload is bound again instead of copied.
  if(reusePreviousData)
  {
    return frameResources.previousConstantBufferOffset;
  }

  // Append
  // The buffer is sized for setsMaxNum maximum-size blocks, so wrapping to the start should not happen within one frame; it guards against overrunning the mapping if it ever does.

  const uint32_t alignedConstantDataSize = AlignTo(constantDataSize, m_ConstantBufferAlignment);

  if(frameResources.constantBufferOffset + alignedConstantDataSize > frameResources.constantBuffer.bufferSize)
  {
    frameResources.constantBufferOffset = 0;
  }

  const uint32_t currentOffset = frameResources.constantBufferOffset;

  std::memcpy(frameResources.constantBuffer.mapping + currentOffset, constantData, constantDataSize);

  rtpt::CheckVk(m_Resources->FlushBuffer(frameResources.constantBuffer, currentOffset, constantDataSize), "ResourceAllocator::FlushBuffer(NRD constants)");

  frameResources.constantBufferOffset         += alignedConstantDataSize;
  frameResources.previousConstantBufferOffset  = currentOffset;

  return currentOffset;
}

void NrdDenoiser::UpdateResourceSet(VkDescriptorSet resourceSet, const nrd::DispatchDesc& dispatchDesc, const DenoiserResources& denoiserInputs)
{
  const nrd::PipelineDesc& pipelineDesc = m_InstanceDesc->pipelines[dispatchDesc.pipelineIndex];

  // Translate NRD resources
  // Each dispatch can bind a different slice of NRD's permanent/transient pools.
  // Translate the NRD resource list into the descriptor set expected by the pipeline selected for this dispatch.
  // imageInfos is reserved up front because each write keeps a pointer to its entry; a reallocation would leave those pointers dangling.

  std::vector<VkDescriptorImageInfo> imageInfos;
  std::vector<VkWriteDescriptorSet>  writes;

  imageInfos.reserve(dispatchDesc.resourcesNum);
  writes.reserve(dispatchDesc.resourcesNum);

  // The dispatch's resource list is flat and in range order, so one running index walks it while each range assigns consecutive bindings of its type.

  uint32_t resourceIndex = 0;
  uint32_t sampledBindingIndex = 0;
  uint32_t storageBindingIndex = 0;

  for(uint32_t rangeIndex = 0; rangeIndex < pipelineDesc.resourceRangesNum; ++rangeIndex)
  {
    const nrd::ResourceRangeDesc& resourceRange = pipelineDesc.resourceRanges[rangeIndex];

    for(uint32_t descriptorIndex = 0; descriptorIndex < resourceRange.descriptorsNum; ++descriptorIndex)
    {
      const nrd::ResourceDesc& resourceDesc = dispatchDesc.resources[resourceIndex++];
      const rtpt::Image&       image = ResolveDispatchImage(resourceDesc.type, resourceDesc.indexInPool, denoiserInputs);

      VkDescriptorImageInfo descriptorImageInfo {
          .sampler     = VK_NULL_HANDLE,
          .imageView   = image.descriptor.imageView,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };

      imageInfos.push_back(descriptorImageInfo);

      uint32_t binding = 0;
      VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;

      // The binding bases match those used to build m_ResourceSetLayout.
      if(resourceRange.descriptorType == nrd::DescriptorType::TEXTURE)
      {
        binding = m_LibraryDesc->spirvBindingOffsets.textureOffset + m_InstanceDesc->resourcesBaseRegisterIndex + sampledBindingIndex++;
        descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      }
      else
      {
        binding = m_LibraryDesc->spirvBindingOffsets.storageTextureAndBufferOffset + m_InstanceDesc->resourcesBaseRegisterIndex + storageBindingIndex++;
        descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      }

      writes.push_back(VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = resourceSet,
          .dstBinding      = binding,
          .descriptorCount = 1,
          .descriptorType  = descriptorType,
          .pImageInfo      = &imageInfos.back(),
      });
    }
  }

  vkUpdateDescriptorSets(m_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void NrdDenoiser::DispatchNrd(VkCommandBuffer cmd, FrameResources& frameResources, const DenoiserResources& denoiserInputs)
{
  const nrd::DispatchDesc* dispatchDescs    = nullptr;
  uint32_t                 dispatchDescsNum = 0;
  const nrd::Identifier    denoiserIdentifier = kDenoiserIdentifier;

  // Query dispatches
  // Kept out of assert() for the same reason as the settings calls in PrepareFrame: under NDEBUG the call would disappear, dispatchDescsNum would stay 0, and the loop below would record nothing at all.
  // That leaves the output images at whatever they last held, which reads as a black image rather than as a failure.

  const nrd::Result dispatchResult = nrd::GetComputeDispatches(*m_Instance, &denoiserIdentifier, 1, dispatchDescs, dispatchDescsNum);
  assert(IsNrdSuccess(dispatchResult));

  if(!IsNrdSuccess(dispatchResult) || dispatchDescs == nullptr)
  {
    return;
  }

  // Record dispatches
  // NRD decides the compute-pass order for the active denoiser. Vulkan only records the described pipelines, descriptors, dynamic constants, and barriers.

  for(uint32_t dispatchIndex = 0; dispatchIndex < dispatchDescsNum; ++dispatchIndex)
  {
    const nrd::DispatchDesc& dispatchDesc = dispatchDescs[dispatchIndex];
    VkDescriptorSet          resourceSet  = AllocateDescriptorSet(frameResources, m_ResourceSetLayout);

    UpdateResourceSet(resourceSet, dispatchDesc, denoiserInputs);

    const uint32_t dynamicOffset = UploadConstantData(frameResources, dispatchDesc.constantBufferData, dispatchDesc.constantBufferDataSize, dispatchDesc.constantBufferDataMatchesPreviousDispatch);

    const std::array<VkDescriptorSet, 2> descriptorSets {
        resourceSet,
        frameResources.frameSet,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipelines[dispatchDesc.pipelineIndex]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data(), 1, &dynamicOffset);
    vkCmdDispatch(cmd, dispatchDesc.gridWidth, dispatchDesc.gridHeight, 1);

    // Each pass may read what the previous one wrote, so the writes are made visible before the next dispatch.
    InsertComputeBarrier(cmd);
  }
}

const rtpt::Image& NrdDenoiser::ResolveDispatchImage(nrd::ResourceType resourceType, uint16_t poolIndex, const DenoiserResources& denoiserInputs) const
{
  // Only the resource types REBLUR_DIFFUSE_SPECULAR requests with the inputs this integration provides are mapped. Anything else is an integration error.
  switch(resourceType)
  {
    case nrd::ResourceType::IN_MV:
      return denoiserInputs.GetMotionVectorsImage();
    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
      return denoiserInputs.GetNormalRoughnessImage();
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
      // Release builds fall back to the diffuse output image rather than returning an invalid reference.
      assert(false && "Unsupported NRD resource type in path-tracer integration");
      return m_DiffuseOutputImage;
  }
}

void NrdDenoiser::TransitionImageToGeneral(VkCommandBuffer cmd, rtpt::Image& image, VkPipelineStageFlags2 dstStageMask) const
{
  // Images already tracked as GENERAL need no barrier.
  if(image.image == VK_NULL_HANDLE || image.descriptor.imageLayout == VK_IMAGE_LAYOUT_GENERAL)
  {
    return;
  }

  // Transition
  // The source stage and access are NONE, so no earlier work on the image is synchronized against. Images normally reach this point freshly allocated in UNDEFINED, whose contents are discarded anyway.

  const VkImageMemoryBarrier2 imageBarrier {
      .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask  = dstStageMask,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
      .oldLayout     = image.descriptor.imageLayout,
      .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .image         = image.image,
      .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
  };

  const VkDependencyInfo dependencyInfo {
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers    = &imageBarrier,
  };

  vkCmdPipelineBarrier2(cmd, &dependencyInfo);

  // Recording the new layout lets later calls skip the barrier.
  image.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

void NrdDenoiser::InsertComputeBarrier(VkCommandBuffer cmd) const
{
  // Compute-to-compute memory barrier
  // Declares that storage writes from the previous compute dispatch are read by the next one, so the ordering between passes is explicit.

  const VkMemoryBarrier2 memoryBarrier {
      .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
  };

  const VkDependencyInfo dependencyInfo {
      .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers    = &memoryBarrier,
  };

  vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

VkFormat NrdDenoiser::ToVkFormat(nrd::Format format)
{
  // Format mapping
  // NRD names packed formats in R-to-A order while Vulkan names them from the most significant bits, so the packed cases appear reversed, e.g. R10_G10_B10_A2 is A2B10G10R10.

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

void NrdDenoiser::CopyMatrix(glm::mat4 matrix, float (&destination)[16])
{
  // glm and NRD both store matrices column-major, so a straight copy preserves the layout.
  std::memcpy(destination, glm::value_ptr(matrix), sizeof(destination));
}

}  // namespace rtpt
