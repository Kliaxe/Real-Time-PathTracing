#pragma once

#include <cstdint>
#include <vector>

#include <volk.h>

#include "PathTracing/Common/ResolveMode.h"
#include "DenoiserResources.h"

namespace rtpt
{

// NrdComposePass
// Resolves REBLUR's denoised diffuse/specular outputs back into the active renderer's HDR target, remodulating the material factors the shaders divided out and serving the denoiser debug views.
// It is a separate class from NrdDenoiser because it has its own shader, NrdCompose.hlsl, with its own pipeline and descriptor contract; NRD itself never sees this pass.
// One descriptor set is kept per frame slot and rewritten when that slot records again, so nothing here is allocated per frame.

class NrdComposePass
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
  // Everything one compose dispatch binds. All image views must already be in GENERAL layout; NrdDenoiser establishes that for the images it owns or tracks.

  struct RecordInput
  {
    // Guides and noisy signals written by the renderer; the debug views show the noisy signals next to the denoised ones.
    const DenoiserResources* denoiserInputs = nullptr;

    // NRD's OUT_DIFF_RADIANCE_HITDIST and OUT_SPEC_RADIANCE_HITDIST.
    VkImageView denoisedDiffuseImageView  = VK_NULL_HANDLE;
    VkImageView denoisedSpecularImageView = VK_NULL_HANDLE;

    // The renderer's undenoised beauty, which fills the background NRD does not denoise.
    VkImageView rawBeautyImageView = VK_NULL_HANDLE;

    // Storage image the composed result is written to.
    VkImageView outputImageView = VK_NULL_HANDLE;

    // Selects what the shader writes to the output.
    DenoiserDebugView debugView = DenoiserDebugView::eFinal;

    // Whether the shader multiplies the material factors back in; must match whether the renderer's shaders divided them out.
    bool enableMaterialDemodulation = false;

    // Resolution of the output; the dispatch covers it in whole groups.
    VkExtent2D viewportSize = {};

    // Frame slot being recorded; selects which descriptor set is rewritten and bound.
    uint32_t frameSlot = 0;
  };

  explicit NrdComposePass(const CreateInfo& createInfo);

  void Initialize();

  void Destroy();

  bool IsReady() const;

  void Record(VkCommandBuffer cmd, const RecordInput& input);

private:

  // PushConstants
  // CPU mirror of ResolvePushConstants in NrdCompose.hlsl; sizes the pipeline layout's range and is what Record pushes, so the two can't drift apart.

  struct PushConstants
  {
    // DenoiserDebugView value.
    uint32_t debugView = 0;

    // 1 when the compose shader should remodulate by the material factors.
    uint32_t useMaterialDemodulation = 0;
  };

  void CreateDescriptorSetLayout();

  void CreatePipelineLayout();

  void CreatePipeline();

  void CreateDescriptorSets();

  void UpdateDescriptorSet(VkDescriptorSet descriptorSet, const RecordInput& input) const;

  // Device every Vulkan object here is created on.
  VkDevice m_Device = VK_NULL_HANDLE;

  // Number of descriptor sets; fixed at construction.
  uint32_t m_FrameSlotCount = 0;

  // Only set of the compose pipeline; mirrors the bindings in NrdCompose.hlsl.
  VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;

  // Compose pipeline layout, with the compose push constants.
  VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

  // Compute pipeline for NrdCompose.hlsl.
  VkPipeline m_Pipeline = VK_NULL_HANDLE;

  // Holds one set per frame slot for the lifetime of the pass; never reset.
  VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;

  // One set per frame slot, rewritten by Record with that frame's views.
  std::vector<VkDescriptorSet> m_DescriptorSets;
};

}  // namespace rtpt
