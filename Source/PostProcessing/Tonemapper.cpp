#include "Tonemapper.h"

#include "Framework/Vulkan/Pipelines.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/geometric.hpp>

namespace rtpt
{
namespace
{

// Per-channel gains that correct a source white point at temperature and tint back to neutral, normalized to unit luminance.
glm::vec3 WhiteBalanceGains(float temperature, float tint)
{
  // Neutral point
  // These constants equal the TonemapperSettings defaults, so default settings give identity gains.
  // Inputs are clamped to the same ranges the UI sliders offer.

  constexpr float kNeutralTemperature = 6506.11144f;
  constexpr float kNeutralTint        = 3.25895312e-3f;

  const float boundedTemperature = std::clamp(temperature, 1000.0f, 15000.0f);
  const float temperatureStops   = std::log2(boundedTemperature / kNeutralTemperature);

  // Temperature
  // Cooler source white points need a stronger blue correction than warm source white points need red.
  // Work in stops so the control remains smooth over its full logarithmic range.

  const float blueSlope = temperatureStops < 0.0f ? 1.25f : 0.60f;

  glm::vec3 gains {
      std::exp2(0.12f * temperatureStops),
      1.0f,
      std::exp2(-blueSlope * temperatureStops),
  };

  // Tint
  // Tint trades green against red and blue, also in stops.

  const float tintOffset = std::clamp(tint, -0.05f, 0.05f) - kNeutralTint;
  const float tintStops  = 6.0f * tintOffset;

  gains[0] *= std::exp2(0.5f * tintStops);
  gains[1] *= std::exp2(-tintStops);
  gains[2] *= std::exp2(0.5f * tintStops);

  // Normalization
  // Dividing by Rec. 709 luminance normalizes away unintended exposure changes from the white balance.

  constexpr glm::vec3 kLuminanceWeights { 0.2126f, 0.7152f, 0.0722f };

  return gains / glm::dot(gains, kLuminanceWeights);
}

// Diagonal matrix combining exposure with white balance, pushed as TonemapperSettings::inputMatrix.
glm::mat3 ColorCorrectionMatrix(float exposure, float temperature, float tint)
{
  const glm::vec3 gains = exposure * WhiteBalanceGains(temperature, tint);

  return glm::mat3(gains[0], 0.0f, 0.0f, 0.0f, gains[1], 0.0f, 0.0f, 0.0f, gains[2]);
}

}  // namespace

VkResult Tonemapper::Initialize(VkDevice device, std::span<const uint32_t> spirv)
{
  if(device == VK_NULL_HANDLE || m_Device != VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  m_Device = device;

  // Pipeline objects
  // Created in dependency order. Any failure calls Destroy, which releases whatever already exists and returns the tonemapper to uninitialized.
  // Binding 0 is the sampled HDR input and binding 1 the LDR storage output, matching Tonemap.hlsl.

  rtpt::DescriptorBindings bindings;

  bindings.Add(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
  bindings.Add(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);

  VkResult result = m_Descriptors.Initialize(m_Device, bindings, 0, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

  if(result != VK_SUCCESS)
  {
    Destroy();
    return result;
  }

  const VkPushConstantRange pushConstantRange {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset     = 0,
      .size       = sizeof(TonemapperSettings),
  };

  const VkPipelineLayoutCreateInfo layoutInfo {
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = 1,
      .pSetLayouts            = m_Descriptors.LayoutPtr(),
      .pushConstantRangeCount = 1,
      .pPushConstantRanges    = &pushConstantRange,
  };

  result = vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_PipelineLayout);

  if(result != VK_SUCCESS)
  {
    Destroy();
    return result;
  }

  result = rtpt::CreateComputePipeline(m_Device, m_PipelineLayout, spirv, m_Pipeline);

  if(result != VK_SUCCESS)
  {
    Destroy();
  }

  return result;
}

void Tonemapper::Destroy()
{
  if(m_Device == VK_NULL_HANDLE)
  {
    return;
  }

  // Release
  // Vulkan destroy calls ignore null handles, so this also works after a partial Initialize.

  vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
  vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);

  m_Descriptors.Destroy();

  m_Pipeline       = VK_NULL_HANDLE;
  m_PipelineLayout = VK_NULL_HANDLE;
  m_Device         = VK_NULL_HANDLE;
}

void Tonemapper::Run(VkCommandBuffer cmd, VkExtent2D extent, const TonemapperSettings& settings, const VkDescriptorImageInfo& input, const VkDescriptorImageInfo& output)
{
  // Settings
  // The color correction matrix is rebuilt from the current exposure and white balance on every dispatch, replacing the caller's inputMatrix.

  TonemapperSettings pushedSettings = settings;

  pushedSettings.inputMatrix = ColorCorrectionMatrix(settings.exposure, settings.temperature, settings.tint);

  vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushedSettings), &pushedSettings);

  // Dispatch
  // Input and output are bound as push descriptors. Group counts round up to cover the image with Tonemap.hlsl's 16x16 work groups; the shader skips pixels past the edge.

  std::array writes { m_Descriptors.MakeWrite(0), m_Descriptors.MakeWrite(1) };

  writes[0].pImageInfo = &input;
  writes[1].pImageInfo = &output;

  vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, static_cast<uint32_t>(writes.size()), writes.data());
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
  vkCmdDispatch(cmd, (extent.width + 15u) / 16u, (extent.height + 15u) / 16u, 1);
}

}  // namespace rtpt
