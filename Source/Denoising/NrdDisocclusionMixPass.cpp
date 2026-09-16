#include "NrdDisocclusionMixPass.h"

#include <algorithm>
#include <array>
#include <iterator>

#include "Framework/Vulkan/Diagnostics.h"

#include "Generated/Shaders/NrdDisocclusionMix.hlsl.main.h"

namespace rtpt
{

namespace
{

// Must match [numthreads] in NrdDisocclusionMix.hlsl.
constexpr uint32_t kGroupSize = 8;

// Bindings 0-1 are the sampled guides and 2 is the storage output, matching the declarations in NrdDisocclusionMix.hlsl.
constexpr uint32_t kSampledImageCount = 2;
constexpr uint32_t kStorageImageCount = 1;

VkShaderModuleCreateInfo GetShaderCode()
{
  // The shader is compiled at build time and embedded as a SPIR-V word array.
  return VkShaderModuleCreateInfo {
      .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = std::size(NrdDisocclusionMix_hlsl) * sizeof(uint32_t),
      .pCode    = NrdDisocclusionMix_hlsl,
  };
}

}  // namespace

NrdDisocclusionMixPass::NrdDisocclusionMixPass(const CreateInfo& createInfo)
    : m_Device(createInfo.device)
    , m_FrameSlotCount(createInfo.frameSlotCount)
{
}

void NrdDisocclusionMixPass::Initialize()
{
  // Missing dependencies or a repeat call leave the pass not ready rather than failing; NrdDenoiser folds IsReady into its own.
  if(m_Device == VK_NULL_HANDLE || m_FrameSlotCount == 0 || m_Pipeline != VK_NULL_HANDLE)
  {
    return;
  }

  CreateDescriptorSetLayout();
  CreatePipelineLayout();
  CreatePipeline();
  CreateDescriptorSets();
}

void NrdDisocclusionMixPass::Destroy()
{
  // Destroying the pool frees every set allocated from it, and destroying VK_NULL_HANDLE is a no-op, so this is safe after a partial Initialize.

  vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
  vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
  vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
  vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, nullptr);

  m_DescriptorSets.clear();

  m_DescriptorPool      = VK_NULL_HANDLE;
  m_Pipeline            = VK_NULL_HANDLE;
  m_PipelineLayout      = VK_NULL_HANDLE;
  m_DescriptorSetLayout = VK_NULL_HANDLE;
}

bool NrdDisocclusionMixPass::IsReady() const
{
  return m_Pipeline != VK_NULL_HANDLE && !m_DescriptorSets.empty();
}

void NrdDisocclusionMixPass::Record(VkCommandBuffer cmd, const RecordInput& input)
{
  if(!IsReady() || cmd == VK_NULL_HANDLE || input.denoiserInputs == nullptr || input.viewportSize.width == 0 || input.viewportSize.height == 0)
  {
    return;
  }

  // This slot's previous frame has finished by the time the slot records again, so its set can be rewritten in place.

  const uint32_t        frameIndex    = std::min(input.frameSlot, uint32_t(m_DescriptorSets.size() - 1));
  const VkDescriptorSet descriptorSet = m_DescriptorSets[frameIndex];

  UpdateDescriptorSet(descriptorSet, input);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
  vkCmdDispatch(cmd, (input.viewportSize.width + kGroupSize - 1) / kGroupSize, (input.viewportSize.height + kGroupSize - 1) / kGroupSize, 1);
}

void NrdDisocclusionMixPass::CreateDescriptorSetLayout()
{
  const std::array<VkDescriptorSetLayoutBinding, 3> bindings {{
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

  rtpt::CheckVk(vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout), "vkCreateDescriptorSetLayout(NRD disocclusion mix)");
}

void NrdDisocclusionMixPass::CreatePipelineLayout()
{
  const VkPipelineLayoutCreateInfo pipelineLayoutInfo {
      .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts    = &m_DescriptorSetLayout,
  };

  rtpt::CheckVk(vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout), "vkCreatePipelineLayout(NRD disocclusion mix)");
}

void NrdDisocclusionMixPass::CreatePipeline()
{
  // The shader module is only needed until pipeline creation, so it is destroyed right after.

  const VkShaderModuleCreateInfo shaderCode   = GetShaderCode();
  VkShaderModule                 shaderModule = VK_NULL_HANDLE;

  rtpt::CheckVk(vkCreateShaderModule(m_Device, &shaderCode, nullptr, &shaderModule), "vkCreateShaderModule(NRD disocclusion mix)");

  const VkComputePipelineCreateInfo pipelineInfo {
      .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage  = VkPipelineShaderStageCreateInfo {
          .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
          .module = shaderModule,
          .pName  = "main",
      },
      .layout = m_PipelineLayout,
  };

  rtpt::CheckVk(vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline), "vkCreateComputePipelines(NRD disocclusion mix)");

  vkDestroyShaderModule(m_Device, shaderModule, nullptr);
}

void NrdDisocclusionMixPass::CreateDescriptorSets()
{
  // Descriptor pool
  // Sized for exactly one set per frame slot. The sets live as long as the pass, so the pool is never reset.

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

  rtpt::CheckVk(vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool), "vkCreateDescriptorPool(NRD disocclusion mix)");

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

  rtpt::CheckVk(vkAllocateDescriptorSets(m_Device, &allocInfo, m_DescriptorSets.data()), "vkAllocateDescriptorSets(NRD disocclusion mix)");
}

void NrdDisocclusionMixPass::UpdateDescriptorSet(VkDescriptorSet descriptorSet, const RecordInput& input) const
{
  const DenoiserResources& denoiserInputs = *input.denoiserInputs;

  const VkDescriptorImageInfo normalRoughnessImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetNormalRoughnessImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo viewZImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetViewZImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const VkDescriptorImageInfo outputImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = denoiserInputs.GetDisocclusionThresholdMixImage().descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const std::array<VkWriteDescriptorSet, 3> writes {{
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 0,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &normalRoughnessImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 1,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo      = &viewZImageInfo,
      },
      VkWriteDescriptorSet {
          .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet          = descriptorSet,
          .dstBinding      = 2,
          .descriptorCount = 1,
          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo      = &outputImageInfo,
      },
  }};

  vkUpdateDescriptorSets(m_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

}  // namespace rtpt
