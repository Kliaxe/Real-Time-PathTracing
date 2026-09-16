#pragma once

#include <cstdint>
#include <vector>

#include <volk.h>

#include "DenoiserResources.h"
#include "Framework/Vulkan/GpuResources.h"

namespace rtpt
{

// RayReconstructionInputPass
// Converts the renderer's NRD guide buffers and noisy radiance into the inputs DLSS Ray Reconstruction reads, right before it evaluates.
// It is its own class because it has its own shader, RayReconstructionInputs.hlsl, with its own pipeline and descriptor contract; RayReconstructionDenoiser owns it and the images it writes, the same way NrdDenoiser owns its compose pass.
// Converting in a pass rather than in the renderers keeps a single set of denoiser signals in both renderers, so choosing a denoiser never changes what they trace or write.

class RayReconstructionInputPass
{
public:

  // CreateInfo
  // Device-level dependencies fixed for the lifetime of the pass.

  struct CreateInfo
  {
    // Device the layouts, pipeline, and descriptor pool are created on.
    VkDevice device = VK_NULL_HANDLE;

    // Number of in-flight frame slots. One descriptor set is created per slot so a set is only rewritten once that slot's previous frame has finished.
    uint32_t frameSlotCount = 0;
  };

  // Outputs
  // The images the pass writes. All must match the viewport size and already be in GENERAL layout.

  struct Outputs
  {
    // Sanitized, clamped noisy radiance.
    const rtpt::Image* color = nullptr;
    // Diffuse reflectance of the primary surface.
    const rtpt::Image* diffuseAlbedo = nullptr;
    // Pre-integrated specular reflectance of the primary surface.
    const rtpt::Image* specularAlbedo = nullptr;
    // World-space normal in xyz, linear roughness in w.
    const rtpt::Image* normalRoughness = nullptr;
    // Hardware depth in [0, 1].
    const rtpt::Image* depth = nullptr;
    // Screen-UV motion of reflections.
    const rtpt::Image* specularMotionVectors = nullptr;
    // Screen-UV motion of the primary surface, with the background's camera rotation filled in.
    const rtpt::Image* motionVectors = nullptr;
  };

  // RecordInput
  // Everything one dispatch binds and pushes.

  struct RecordInput
  {
    // Supplies the renderer's guide buffers and denoiser signals.
    const DenoiserResources* denoiserInputs = nullptr;

    // The renderer's noisy radiance for this frame.
    const rtpt::Image*       color = nullptr;

    // Images written by the pass.
    Outputs                  outputs {};

    // Device address of the scene info buffer, for the camera and jitter.
    VkDeviceAddress          sceneInfoAddress = 0;

    // Brightest-channel ceiling for the noisy radiance; zero disables it.
    float                    radianceClamp = 0.0f;

    // Resolution of the images; the dispatch covers it in whole groups.
    VkExtent2D               viewportSize = {};

    // Frame slot being recorded; selects which descriptor set is rewritten and bound.
    uint32_t                 frameSlot = 0;
  };

  explicit RayReconstructionInputPass(const CreateInfo& createInfo);

  void Initialize();

  void Destroy();

  bool IsReady() const;

  void Record(VkCommandBuffer cmd, const RecordInput& input);

private:

  void CreateDescriptorSetLayout();

  void CreatePipelineLayout();

  void CreatePipeline();

  void CreateDescriptorSets();

  void UpdateDescriptorSet(VkDescriptorSet descriptorSet, const RecordInput& input) const;

  // Device every Vulkan object here is created on.
  VkDevice m_Device = VK_NULL_HANDLE;

  // Number of descriptor sets; fixed at construction.
  uint32_t m_FrameSlotCount = 0;

  // Only set of the pipeline; mirrors the bindings in RayReconstructionInputs.hlsl.
  VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;

  // Pipeline layout with the single set and the RayReconstructionInputsPushConstant range.
  VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

  // Compute pipeline for RayReconstructionInputs.hlsl.
  VkPipeline m_Pipeline = VK_NULL_HANDLE;

  // Holds one set per frame slot for the lifetime of the pass; never reset.
  VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;

  // One set per frame slot, rewritten by Record with that frame's views.
  std::vector<VkDescriptorSet> m_DescriptorSets;
};

}  // namespace rtpt
