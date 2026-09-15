#include "SkyRenderer.h"

#include "Framework/Vulkan/Pipelines.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace rtpt
{

// SkySimpleParameters is shared with HLSL, so its size is part of the CPU/GPU layout contract.
static_assert(sizeof(shaderio::SkySimpleParameters) == 112);

SkyRenderer::~SkyRenderer()
{
  Destroy();
}

VkResult SkyRenderer::Initialize(VkDevice device, std::span<const uint32_t> spirv)
{
  // Must match SkyPushConstants in Sky.hlsl: 112 bytes of sky parameters followed by a 64-byte matrix.
  static_assert(sizeof(PushConstants) == 176);

  if(device == VK_NULL_HANDLE || spirv.empty() || m_Device != VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  m_Device = device;

  // Pipeline objects
  // Created in dependency order. Any failure calls Destroy, which releases whatever already exists and returns the renderer to uninitialized.

  DescriptorBindings bindings;

  bindings.Add(shaderio::SkyBindings::eSkyOutImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);

  VkResult result = m_Descriptors.Initialize(m_Device, bindings, 0, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

  if(result != VK_SUCCESS)
  {
    Destroy();
    return result;
  }

  const VkPushConstantRange pushConstantRange {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset     = 0,
      .size       = sizeof(PushConstants),
  };

  result = CreatePipelineLayout(m_Device, m_PipelineLayout, std::span(m_Descriptors.LayoutPtr(), 1), std::span(&pushConstantRange, 1));

  if(result != VK_SUCCESS)
  {
    Destroy();
    return result;
  }

  result = CreateComputePipeline(m_Device, m_PipelineLayout, spirv, m_Pipeline);

  if(result != VK_SUCCESS)
  {
    Destroy();
  }

  return result;
}

void SkyRenderer::Destroy()
{
  if(m_Device == VK_NULL_HANDLE)
  {
    return;
  }

  if(m_Pipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
  }

  if(m_PipelineLayout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
  }

  m_Descriptors.Destroy();

  m_Pipeline       = VK_NULL_HANDLE;
  m_PipelineLayout = VK_NULL_HANDLE;
  m_Device         = VK_NULL_HANDLE;
}

void SkyRenderer::Run(VkCommandBuffer commandBuffer, VkExtent2D extent, const glm::mat4& view, const glm::mat4& projection, const shaderio::SkySimpleParameters& parameters, const VkDescriptorImageInfo& output) const
{
  // Push constants
  // Clearing the view's translation leaves only its rotation, so the inverse maps screen positions to directions that do not depend on the camera position.

  glm::mat4 rotationOnlyView = view;

  rotationOnlyView[3] = { 0.0F, 0.0F, 0.0F, 1.0F };

  const PushConstants pushConstants { .parameters = parameters, .transform = glm::inverse(projection * rotationOnlyView) };

  vkCmdPushConstants(commandBuffer, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants), &pushConstants);

  // Dispatch
  // The output is bound as a push descriptor. Group counts round up to cover the image with Sky.hlsl's 16x16 work groups; the shader skips pixels past the edge.

  VkWriteDescriptorSet write = m_Descriptors.MakeWrite(shaderio::SkyBindings::eSkyOutImage);

  write.pImageInfo = &output;

  vkCmdPushDescriptorSetKHR(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &write);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
  vkCmdDispatch(commandBuffer, (extent.width + 15u) / 16u, (extent.height + 15u) / 16u, 1);
}

}  // namespace rtpt
