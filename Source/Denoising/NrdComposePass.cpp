#include "NrdComposePass.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <iterator>

#include "Framework/Vulkan/Diagnostics.h"

#include "Generated/Shaders/NrdCompose.hlsl.main.h"

namespace rtpt
{

namespace
{

// Must match [numthreads] in NrdCompose.hlsl.
constexpr uint32_t kComposeGroupSize = 8;

// Bindings 0-9 are sampled inputs and 10 is the storage output, matching the declarations in NrdCompose.hlsl.
constexpr uint32_t kSampledImageCount = 10;
constexpr uint32_t kStorageImageCount = 1;

VkShaderModuleCreateInfo GetComposeShaderCode()
{
  // The compose shader is compiled at build time and embedded as a SPIR-V word array.
  return VkShaderModuleCreateInfo {
      .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = std::size(NrdCompose_hlsl) * sizeof(uint32_t),
      .pCode    = NrdCompose_hlsl,
  };
}

}  // namespace

NrdComposePass::NrdComposePass(const CreateInfo& createInfo)
    : m_Device(createInfo.device)
    , m_FrameSlotCount(createInfo.frameSlotCount)
{
}

void NrdComposePass::Initialize()
{
  // Missing dependencies or a repeat call leave the pass not ready rather than failing; NrdDenoiser folds IsReady into its own.
  if(m_Device == VK_NULL_HANDLE || m_FrameSlotCount == 0 || m_Pipeline != VK_NULL_HANDLE)
  {
    return;
  }

  // Order follows dependencies: the pipeline layout uses the set layout, the pipeline uses the pipeline layout, and the sets are allocated against the set layout.

  CreateDescriptorSetLayout();
  CreatePipelineLayout();
  CreatePipeline();
  CreateDescriptorSets();
}

void NrdComposePass::Destroy()
{
  VkDevice device = m_Device;

  // Destroying the pool frees every set allocated from it, so the sets need no separate release. It goes before the layout it was allocated against.
  // Destroying VK_NULL_HANDLE is a no-op, so this is safe after a partial or failed Initialize.

  vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr);
  vkDestroyPipeline(device, m_Pipeline, nullptr);
  vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
  vkDestroyDescriptorSetLayout(device, m_DescriptorSetLayout, nullptr);

  // Nulled handles make IsReady false and a repeated Destroy harmless.

  m_DescriptorSets.clear();

  m_DescriptorPool      = VK_NULL_HANDLE;
  m_Pipeline            = VK_NULL_HANDLE;
  m_PipelineLayout      = VK_NULL_HANDLE;
  m_DescriptorSetLayout = VK_NULL_HANDLE;
}

bool NrdComposePass::IsReady() const
{
  return m_Pipeline != VK_NULL_HANDLE && !m_DescriptorSets.empty();
}

void NrdComposePass::Record(VkCommandBuffer cmd, const RecordInput& input)
{
  if(!IsReady() || cmd == VK_NULL_HANDLE || input.denoiserInputs == nullptr || input.rawBeautyImageView == VK_NULL_HANDLE || input.outputImageView == VK_NULL_HANDLE || input.viewportSize.width == 0 || input.viewportSize.height == 0)
  {
    return;
  }

  // Descriptors
  // This slot's previous frame has finished by the time the slot records again, so its set can be rewritten in place rather than allocated fresh.

  const uint32_t        frameIndex    = std::min(input.frameSlot, uint32_t(m_DescriptorSets.size() - 1));
  const VkDescriptorSet descriptorSet = m_DescriptorSets[frameIndex];

  UpdateDescriptorSet(descriptorSet, input);

  // Dispatch
  // The group count rounds up so the whole viewport is covered; the shader discards invocations past the edge.

  const PushConstants pushConstants {
      .debugView               = static_cast<uint32_t>(input.debugView),
      .useMaterialDemodulation = input.enableMaterialDemodulation ? 1u : 0u,
  };

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
  vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &pushConstants);
  vkCmdDispatch(cmd, (input.viewportSize.width + kComposeGroupSize - 1) / kComposeGroupSize, (input.viewportSize.height + kComposeGroupSize - 1) / kComposeGroupSize, 1);
}

void NrdComposePass::CreateDescriptorSetLayout()
{
  // Compose set
  // Bindings 0-9 are the sampled inputs and 10 is the output, matching the declarations in NrdCompose.hlsl.

  const std::array<VkDescriptorSetLayoutBinding, 11> bindings {{
      VkDescriptorSetLayoutBinding {
          .binding         = 0,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 2,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 3,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 4,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 5,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 6,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 7,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 8,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 9,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      VkDescriptorSetLayoutBinding {
          .binding         = 10,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
          .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
      },
  }};

  static_assert(std::tuple_size_v<decltype(bindings)> == kSampledImageCount + kStorageImageCount);

  const VkDescriptorSetLayoutCreateInfo layoutInfo {
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings    = bindings.data(),
  };

  rtpt::CheckVk(vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout), "vkCreateDescriptorSetLayout(NRD compose)");
}

void NrdComposePass::CreatePipelineLayout()
{
  // Pipeline layout
  // One set plus the push constants; PushConstants is the same struct Record pushes, so the range can't disagree with the recorded data.

  const VkPushConstantRange pushConstantRange {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset     = 0,
      .size       = sizeof(PushConstants),
  };

  const VkPipelineLayoutCreateInfo pipelineLayoutInfo {
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = &m_DescriptorSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges    = &pushConstantRange,
  };

  rtpt::CheckVk(vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout), "vkCreatePipelineLayout(NRD compose)");
}

void NrdComposePass::CreatePipeline()
{
  VkDevice device = m_Device;

  // Compose pipeline
  // The shader module is only needed until pipeline creation, so it is destroyed right after.

  const VkShaderModuleCreateInfo shaderCode   = GetComposeShaderCode();
  VkShaderModule                 shaderModule = VK_NULL_HANDLE;

  rtpt::CheckVk(vkCreateShaderModule(device, &shaderCode, nullptr, &shaderModule), "vkCreateShaderModule(NRD compose)");

  const VkPipelineShaderStageCreateInfo shaderStage {
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shaderModule,
      .pName  = "main",
  };

  const VkComputePipelineCreateInfo pipelineInfo {
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = shaderStage,
      .layout = m_PipelineLayout,
  };

  rtpt::CheckVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline), "vkCreateComputePipelines(NRD compose)");

  vkDestroyShaderModule(device, shaderModule, nullptr);
}

void NrdComposePass::CreateDescriptorSets()
{
  // Descriptor pool
  // Sized for exactly one set per frame slot: ten sampled images and one storage image each. The sets live as long as the pass, so the pool is never reset.

  const VkDescriptorPoolSize poolSizes[] = {
      VkDescriptorPoolSize {
          .type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = m_FrameSlotCount * kSampledImageCount,
      },
      VkDescriptorPoolSize {
          .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = m_FrameSlotCount * kStorageImageCount,
      },
  };

  const VkDescriptorPoolCreateInfo poolInfo {
      .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets       = m_FrameSlotCount,
      .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
      .pPoolSizes    = poolSizes,
  };

  rtpt::CheckVk(vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool), "vkCreateDescriptorPool(NRD compose)");

  // Sets
  // Allocated once, in slot order, so Record indexes them by frame slot directly.

  const std::vector<VkDescriptorSetLayout> layouts(m_FrameSlotCount, m_DescriptorSetLayout);

  const VkDescriptorSetAllocateInfo allocInfo {
      .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool     = m_DescriptorPool,
      .descriptorSetCount = static_cast<uint32_t>(layouts.size()),
      .pSetLayouts        = layouts.data(),
  };

  m_DescriptorSets.assign(m_FrameSlotCount, VK_NULL_HANDLE);

  rtpt::CheckVk(vkAllocateDescriptorSets(m_Device, &allocInfo, m_DescriptorSets.data()), "vkAllocateDescriptorSets(NRD compose)");
}

void NrdComposePass::UpdateDescriptorSet(VkDescriptorSet descriptorSet, const RecordInput& input) const
{
  const DenoiserResources& denoiserInputs = *input.denoiserInputs;

  // Compose descriptors
  // Everything is bound in GENERAL layout, which Denoise established for the images it owns or tracks.

  const VkDescriptorImageInfo diffuseImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = input.denoisedDiffuseImageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo specularImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = input.denoisedSpecularImageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo rawBeautyImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = input.rawBeautyImageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo viewZImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetViewZImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo normalRoughnessImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetNormalRoughnessImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo baseColorMetalnessImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetBaseColorMetalnessImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo motionVectorsImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetMotionVectorsImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo noisyDiffuseImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetDiffuseRadianceHitDistanceImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo noisySpecularImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetSpecularRadianceHitDistanceImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo specularDemodulationFactorImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetSpecularDemodulationFactorImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo outputStorageImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = input.outputImageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  // Binding numbers follow the declaration order in NrdCompose.hlsl, not the order of the image infos above.

  const std::array<VkWriteDescriptorSet, 11> writes {{
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &diffuseImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 1,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &specularImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 2,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &rawBeautyImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 3,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &viewZImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 4,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &normalRoughnessImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 5,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &motionVectorsImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 6,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &noisyDiffuseImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 7,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &noisySpecularImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 8,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &baseColorMetalnessImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 9,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &specularDemodulationFactorImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 10,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo      = &outputStorageImageInfo,
      },
  }};

  vkUpdateDescriptorSets(m_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

}  // namespace rtpt
