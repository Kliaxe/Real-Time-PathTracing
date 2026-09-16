#pragma once

#include <cstdint>
#include <vector>

#include <volk.h>

#include "DenoiserResources.h"

namespace rtpt
{

// NrdDisocclusionMixPass
// Writes NRD's IN_DISOCCLUSION_THRESHOLD_MIX from the renderer's normal and viewZ guides, right before NRD dispatches.
// It is its own class because it has its own shader, NrdDisocclusionMix.hlsl, with its own pipeline and descriptor contract; NrdDenoiser owns it the same way it owns NrdComposePass.
// It runs inside the denoiser rather than in the renderers because the measure compares each pixel with its neighbours, which a ray generation shader writing one pixel at a time cannot see.

class NrdDisocclusionMixPass
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

  // RecordInput
  // Everything one dispatch binds. All three images must already be in GENERAL layout.

  struct RecordInput
  {
    // Supplies the normal and viewZ guides read, and the mix image written.
    const DenoiserResources* denoiserInputs = nullptr;

    // Resolution of the guides; the dispatch covers it in whole groups.
    VkExtent2D viewportSize = {};

    // Frame slot being recorded; selects which descriptor set is rewritten and bound.
    uint32_t frameSlot = 0;
  };

  explicit NrdDisocclusionMixPass(const CreateInfo& createInfo);

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

  // Only set of the pipeline; mirrors the bindings in NrdDisocclusionMix.hlsl.
  VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;

  // Pipeline layout with the single set and no push constants.
  VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

  // Compute pipeline for NrdDisocclusionMix.hlsl.
  VkPipeline m_Pipeline = VK_NULL_HANDLE;

  // Holds one set per frame slot for the lifetime of the pass; never reset.
  VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;

  // One set per frame slot, rewritten by Record with that frame's views.
  std::vector<VkDescriptorSet> m_DescriptorSets;
};

}  // namespace rtpt
