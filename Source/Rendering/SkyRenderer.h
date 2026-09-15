#pragma once

#include "Common/SkyIo.h"
#include "Framework/Vulkan/Descriptors.h"

#include <span>

#include <glm/mat4x4.hpp>

namespace rtpt
{

// SkyRenderer
// Compute pass that fills a storage image with the procedural sky; the rasterizer preview runs it when the scene's sky is enabled.
// The output is bound through a push descriptor set, so no descriptor pool is needed and each dispatch can target a different image.

class SkyRenderer
{
public:

  SkyRenderer() = default;
  SkyRenderer(const SkyRenderer&)            = delete;
  SkyRenderer& operator=(const SkyRenderer&) = delete;
  ~SkyRenderer();

  // Builds the descriptor layout, pipeline layout, and pipeline. Fails if already initialized, and destroys anything partly built on failure.
  VkResult Initialize(VkDevice device, std::span<const uint32_t> spirv);

  void Destroy();

  // Records the sky dispatch into output, which the caller has already transitioned to a storage-writable layout.
  void Run(VkCommandBuffer commandBuffer, VkExtent2D extent, const glm::mat4& view, const glm::mat4& projection, const shaderio::SkySimpleParameters& parameters, const VkDescriptorImageInfo& output) const;

private:

  // PushConstants
  // CPU mirror of SkyPushConstants in Sky.hlsl. Initialize asserts its 176-byte size so the two layouts cannot drift apart silently.

  struct PushConstants
  {
    // Sun and sky parameters for the shared sky evaluation.
    shaderio::SkySimpleParameters parameters;

    // Inverse of projection times the rotation-only view, mapping screen positions to world-space view directions.
    glm::mat4                     transform { 1.0F };
  };

  // Device the pipeline objects belong to. Null until initialized, which also makes Destroy a no-op.
  VkDevice         m_Device = VK_NULL_HANDLE;

  // Push-descriptor layout holding the single output storage image.
  DescriptorPack   m_Descriptors;

  // Layout combining the push descriptor set and the push constant range.
  VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

  // Compute pipeline built from Sky.hlsl.
  VkPipeline       m_Pipeline = VK_NULL_HANDLE;
};

}  // namespace rtpt
