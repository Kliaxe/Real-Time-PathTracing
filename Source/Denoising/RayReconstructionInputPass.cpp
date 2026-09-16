#include "RayReconstructionInputPass.h"

#include <algorithm>
#include <array>
#include <iterator>

#include "Framework/Vulkan/Diagnostics.h"
#include "Shaders/ShaderIo.h"

#include "Generated/Shaders/RayReconstructionInputs.hlsl.main.h"

namespace rtpt
{

namespace
{

// Must match [numthreads] in RayReconstructionInputs.hlsl.
constexpr uint32_t kGroupSize = 8;

// Bindings 0-6 are the sampled renderer images and 7-13 the storage outputs, matching the declarations in RayReconstructionInputs.hlsl.
constexpr uint32_t kSampledImageCount = 7;
constexpr uint32_t kStorageImageCount = 7;
constexpr uint32_t kBindingCount      = kSampledImageCount + kStorageImageCount;

VkShaderModuleCreateInfo GetShaderCode()
{
  // The shader is compiled at build time and embedded as a SPIR-V word array.
  return VkShaderModuleCreateInfo {
      .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = std::size(RayReconstructionInputs_hlsl) * sizeof(uint32_t),
      .pCode    = RayReconstructionInputs_hlsl,
  };
}

// Every image is bound in GENERAL: the renderer wrote its images as storage, and the outputs are storage images here.
VkDescriptorImageInfo DescribeImage(const rtpt::Image& image)
{
  return VkDescriptorImageInfo {
      .sampler     = VK_NULL_HANDLE,
      .imageView   = image.descriptor.imageView,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
}

}  // namespace

RayReconstructionInputPass::RayReconstructionInputPass(const CreateInfo& createInfo)
    : m_Device(createInfo.device)
    , m_FrameSlotCount(createInfo.frameSlotCount)
{
}

void RayReconstructionInputPass::Initialize()
{
  // Missing dependencies or a repeat call leave the pass not ready rather than failing; RayReconstructionDenoiser folds IsReady into its own.
  if(m_Device == VK_NULL_HANDLE || m_FrameSlotCount == 0 || m_Pipeline != VK_NULL_HANDLE)
  {
    return;
  }

  CreateDescriptorSetLayout();
  CreatePipelineLayout();
  CreatePipeline();
  CreateDescriptorSets();
}

void RayReconstructionInputPass::Destroy()
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

bool RayReconstructionInputPass::IsReady() const
{
  return m_Pipeline != VK_NULL_HANDLE && !m_DescriptorSets.empty();
}

void RayReconstructionInputPass::Record(VkCommandBuffer cmd, const RecordInput& input)
{
  const Outputs& outputs = input.outputs;

  const bool outputsBound = outputs.color != nullptr && outputs.diffuseAlbedo != nullptr && outputs.specularAlbedo != nullptr && outputs.normalRoughness != nullptr && outputs.depth != nullptr && outputs.specularMotionVectors != nullptr && outputs.motionVectors != nullptr;

  if(!IsReady() || cmd == VK_NULL_HANDLE || input.denoiserInputs == nullptr || input.color == nullptr || !outputsBound || input.sceneInfoAddress == 0 || input.viewportSize.width == 0 || input.viewportSize.height == 0)
  {
    return;
  }

  // This slot's previous frame has finished by the time the slot records again, so its set can be rewritten in place.

  const uint32_t        frameIndex    = std::min(input.frameSlot, uint32_t(m_DescriptorSets.size() - 1));
  const VkDescriptorSet descriptorSet = m_DescriptorSets[frameIndex];

  UpdateDescriptorSet(descriptorSet, input);

  const shaderio::RayReconstructionInputsPushConstant pushConstant {
      .sceneInfoAddress = reinterpret_cast<shaderio::GltfSceneInfo*>(input.sceneInfoAddress),
      .radianceClamp    = input.radianceClamp,
      ._pad0            = 0.0f,
  };

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
  vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstant), &pushConstant);
  vkCmdDispatch(cmd, (input.viewportSize.width + kGroupSize - 1) / kGroupSize, (input.viewportSize.height + kGroupSize - 1) / kGroupSize, 1);
}

void RayReconstructionInputPass::CreateDescriptorSetLayout()
{
  // Sampled inputs occupy the first bindings and storage outputs the rest, in the shader's order.

  std::array<VkDescriptorSetLayoutBinding, kBindingCount> bindings {};

  for(uint32_t binding = 0; binding < kBindingCount; ++binding)
  {
    bindings[binding] = VkDescriptorSetLayoutBinding {
        .binding         = binding,
        .descriptorType  = binding < kSampledImageCount ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT,
    };
  }

  const VkDescriptorSetLayoutCreateInfo layoutInfo {
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings    = bindings.data(),
  };

  rtpt::CheckVk(vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout), "vkCreateDescriptorSetLayout(Ray Reconstruction inputs)");
}

void RayReconstructionInputPass::CreatePipelineLayout()
{
  const VkPushConstantRange pushConstantRange {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset     = 0,
      .size       = sizeof(shaderio::RayReconstructionInputsPushConstant),
  };

  const VkPipelineLayoutCreateInfo pipelineLayoutInfo {
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = &m_DescriptorSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges    = &pushConstantRange,
  };

  rtpt::CheckVk(vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout), "vkCreatePipelineLayout(Ray Reconstruction inputs)");
}

void RayReconstructionInputPass::CreatePipeline()
{
  // The shader module is only needed until pipeline creation, so it is destroyed right after.

  const VkShaderModuleCreateInfo shaderCode   = GetShaderCode();
  VkShaderModule                 shaderModule = VK_NULL_HANDLE;

  rtpt::CheckVk(vkCreateShaderModule(m_Device, &shaderCode, nullptr, &shaderModule), "vkCreateShaderModule(Ray Reconstruction inputs)");

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

  rtpt::CheckVk(vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline), "vkCreateComputePipelines(Ray Reconstruction inputs)");

  vkDestroyShaderModule(m_Device, shaderModule, nullptr);
}

void RayReconstructionInputPass::CreateDescriptorSets()
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

  rtpt::CheckVk(vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool), "vkCreateDescriptorPool(Ray Reconstruction inputs)");

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

  rtpt::CheckVk(vkAllocateDescriptorSets(m_Device, &allocInfo, m_DescriptorSets.data()), "vkAllocateDescriptorSets(Ray Reconstruction inputs)");
}

void RayReconstructionInputPass::UpdateDescriptorSet(VkDescriptorSet descriptorSet, const RecordInput& input) const
{
  const DenoiserResources& denoiserInputs = *input.denoiserInputs;
  const Outputs&           outputs        = input.outputs;

  // Parallel to the shader's bindings: images[i] is written to binding i.
  const std::array<VkDescriptorImageInfo, kBindingCount> images {
      DescribeImage(*input.color),
      DescribeImage(denoiserInputs.GetNormalRoughnessImage()),
      DescribeImage(denoiserInputs.GetBaseColorMetalnessImage()),
      DescribeImage(denoiserInputs.GetViewZImage()),
      DescribeImage(denoiserInputs.GetSpecularRadianceHitDistanceImage()),
      DescribeImage(denoiserInputs.GetSpecularDemodulationFactorImage()),
      DescribeImage(denoiserInputs.GetMotionVectorsImage()),
      DescribeImage(*outputs.color),
      DescribeImage(*outputs.diffuseAlbedo),
      DescribeImage(*outputs.specularAlbedo),
      DescribeImage(*outputs.normalRoughness),
      DescribeImage(*outputs.depth),
      DescribeImage(*outputs.specularMotionVectors),
      DescribeImage(*outputs.motionVectors),
  };

  std::array<VkWriteDescriptorSet, kBindingCount> writes {};

  for(uint32_t binding = 0; binding < kBindingCount; ++binding)
  {
    writes[binding] = VkWriteDescriptorSet {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = descriptorSet,
        .dstBinding      = binding,
        .descriptorCount = 1,
        .descriptorType  = binding < kSampledImageCount ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo      = &images[binding],
    };
  }

  vkUpdateDescriptorSets(m_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

}  // namespace rtpt
