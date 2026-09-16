#pragma once

#include <cstdint>
#include <vector>

#include <glm/mat4x4.hpp>
#include <volk.h>

#include "PathTracing/Common/ResolveMode.h"
#include "DenoiserResources.h"
#include "NrdComposePass.h"
#include "NrdDisocclusionMixPass.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Shaders/ShaderIo.h"

#include <NRD.h>

namespace rtpt
{

// NrdDenoiser
// Owns the native NRD instance and the Vulkan resources that mirror NRD's image model.
// NRD itself only describes pipelines, descriptors, and dispatches; this class turns those descriptions into Vulkan objects and command-buffer recording.
// The compose step that resolves the denoised diffuse/specular outputs back into the active renderer's HDR target has its own shader, so it lives in the owned NrdComposePass and runs right after NRD's dispatches.
// Each renderer owns one instance, fed by that renderer's DenoiserResources.

class NrdDenoiser
{
public:

  // CreateInfo
  // Device-level dependencies fixed for the lifetime of the denoiser.

  struct CreateInfo
  {
    // Device every pipeline, layout, and pool is created on.
    VkDevice                 device = VK_NULL_HANDLE;

    // Allocates images and constant buffers. Initialize does nothing while this is null.
    rtpt::ResourceAllocator* resources = nullptr;

    // Optional; used only to attach debug names.
    const rtpt::Diagnostics* diagnostics = nullptr;

    // Number of in-flight frame slots. One FrameResources is created per slot so a slot's pool and constants are only reset when that slot records again.
    uint32_t                 frameSlotCount = 0;
  };

  // FrameInput
  // Per-frame state PrepareFrame needs before the renderer writes its noisy signals.

  struct FrameInput
  {
    // Source of the current camera matrices.
    const shaderio::GltfSceneInfo* sceneInfo           = nullptr;

    // Resolution of the renderer's signals this frame.
    VkExtent2D                     viewportSize        = {};

    // Set by the renderer when its history no longer matches, so NRD restarts accumulation.
    bool                           historyInvalidated  = false;

    // Whether the compose pass multiplies the material factors back in; must match whether the shaders divided them out.
    bool                           enableMaterialDemodulation = false;

    // REBLUR tuning. Null keeps the settings applied last.
    const DenoiserSettings*        settings            = nullptr;

    // Frame slot being recorded; selects which FrameResources this frame uses.
    uint32_t                       frameSlot = 0;

    // Time since the previous frame, in milliseconds as nrd::CommonSettings documents. Zero lets NRD measure it with its own wall-clock timer.
    float                          frameTimeMilliseconds = 0.0f;
  };

  explicit NrdDenoiser(const CreateInfo& createInfo);

  void Initialize();

  void Destroy();

  bool IsReady() const;

  void InvalidateHistory();

  void PrepareFrame(const FrameInput& input, const DenoiserResources& denoiserInputs);

  void Denoise(VkCommandBuffer cmd, const DenoiserResources& denoiserInputs, VkImageView rawBeautyImageView, VkImageView outputImageView, DenoiserDebugView debugView, VkExtent2D viewportSize);

  const rtpt::Image& GetDiffuseOutputImage() const;

  const rtpt::Image& GetSpecularOutputImage() const;

private:

  // FrameResources
  // Descriptor and constant storage for one frame slot.
  // NRD issues a variable number of dispatches per frame, each needing its own resource set and constants, so everything here is transient and reset at the start of the slot's next frame.

  struct FrameResources
  {
    // Holds every set allocated for this slot's frame; reset wholesale in BeginFrame.
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    // Set 1 for every NRD dispatch: immutable samplers plus the dynamic constant buffer.
    VkDescriptorSet  frameSet       = VK_NULL_HANDLE;

    // Host-mapped uniform buffer that each dispatch's constants are appended to at aligned offsets.
    rtpt::Buffer     constantBuffer;

    // Next free aligned offset in constantBuffer.
    uint32_t         constantBufferOffset      = 0;

    // Offset of the last upload, reused when NRD reports a dispatch's constants are unchanged.
    uint32_t         previousConstantBufferOffset = 0;
  };

  void CreateVulkanState();

  void DestroyVulkanState();

  void CreateSamplers();

  void CreateDescriptorSetLayouts();

  void CreatePipelineLayout();

  void CreatePipelines();

  void CreateFrameResources();

  void DestroyFrameResources();

  void DestroyViewportResources();

  void EnsureForViewport(VkExtent2D viewportSize);

  void RecreateViewportResources(VkExtent2D viewportSize);

  rtpt::Image CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const;

  void ApplyDenoiserSettings(const DenoiserSettings& settings);

  void UpdateCommonSettings(const FrameInput& input);

  FrameResources& GetCurrentFrameResources();

  void BeginFrame(FrameResources& frameResources);

  void UpdateFrameSet(FrameResources& frameResources);

  VkDescriptorSet AllocateDescriptorSet(FrameResources& frameResources, VkDescriptorSetLayout layout);

  uint32_t UploadConstantData(FrameResources& frameResources, const void* constantData, uint32_t constantDataSize, bool reusePreviousData);

  void UpdateResourceSet(VkDescriptorSet resourceSet, const nrd::DispatchDesc& dispatchDesc, const DenoiserResources& denoiserInputs);

  void DispatchNrd(VkCommandBuffer cmd, FrameResources& frameResources, const DenoiserResources& denoiserInputs);

  const rtpt::Image& ResolveDispatchImage(nrd::ResourceType resourceType, uint16_t poolIndex, const DenoiserResources& denoiserInputs) const;

  void TransitionImageToGeneral(VkCommandBuffer cmd, rtpt::Image& image, VkPipelineStageFlags2 dstStageMask) const;

  void InsertComputeBarrier(VkCommandBuffer cmd) const;

  static VkFormat ToVkFormat(nrd::Format format);

  static void     CopyMatrix(glm::mat4 matrix, float (&destination)[16]);

  // Device every Vulkan object here is created on.
  VkDevice                 m_Device = VK_NULL_HANDLE;

  // Allocates images and constant buffers.
  rtpt::ResourceAllocator* m_Resources = nullptr;

  // Optional debug-name sink; may be null.
  const rtpt::Diagnostics* m_Diagnostics = nullptr;

  // Number of FrameResources entries; fixed at construction.
  uint32_t                 m_FrameSlotCount = 0;

  // Slot recorded this frame, clamped to m_FrameSlotCount - 1.
  uint32_t                 m_CurrentFrameSlot = 0;

  // Resolution of the output and pool images. Zero until the first allocation.
  VkExtent2D               m_ViewportSize {};

  // Pending history reset. Starts true so the first frame clears NRD's history, and stays set until NRD accepts the settings.
  bool                     m_HistoryInvalidated = true;

  // Latched from FrameInput; forwarded to the compose pass, which pushes it to its shader.
  bool                     m_EnableMaterialDemodulation = false;

  // Latched from DenoiserSettings by ApplyDenoiserSettings, because CommonSettings is rebuilt from scratch every frame.
  float                    m_DisocclusionThreshold          = 0.0f;
  float                    m_DisocclusionThresholdAlternate = 0.0f;

  // Whether NRD is told IN_DISOCCLUSION_THRESHOLD_MIX exists, and so whether Denoise writes it.
  bool                     m_EnableDisocclusionThresholdMix = false;

  // Immutable sampler for nrd::Sampler::NEAREST_CLAMP.
  VkSampler m_NearestSampler = VK_NULL_HANDLE;

  // Immutable sampler for nrd::Sampler::LINEAR_CLAMP.
  VkSampler m_LinearSampler  = VK_NULL_HANDLE;

  // Set 0 of the NRD pipelines: the sampled and storage images one dispatch binds.
  VkDescriptorSetLayout m_ResourceSetLayout = VK_NULL_HANDLE;

  // Set 1 of the NRD pipelines: samplers and the dynamic constant buffer.
  VkDescriptorSetLayout m_FrameSetLayout    = VK_NULL_HANDLE;

  // Shared by every NRD pipeline, since NRD uses one binding model for all of them.
  VkPipelineLayout      m_PipelineLayout    = VK_NULL_HANDLE;

  // One compute pipeline per nrd::PipelineDesc, indexed by DispatchDesc::pipelineIndex.
  std::vector<VkPipeline> m_Pipelines;

  // Resolves the denoised outputs into the renderer's target after DispatchNrd; created and destroyed with the rest of the Vulkan state.
  NrdComposePass          m_ComposePass;

  // Writes IN_DISOCCLUSION_THRESHOLD_MIX before DispatchNrd while the mix is enabled; created and destroyed with the rest of the Vulkan state.
  NrdDisocclusionMixPass  m_DisocclusionMixPass;

  // One entry per frame slot.
  std::vector<FrameResources> m_FrameResources;

  // minUniformBufferOffsetAlignment of the device, applied to every dynamic offset. 256 is only the pre-Initialize placeholder.
  uint32_t m_ConstantBufferAlignment = 256;

  // OUT_DIFF_RADIANCE_HITDIST.
  rtpt::Image m_DiffuseOutputImage;

  // OUT_SPEC_RADIANCE_HITDIST.
  rtpt::Image m_SpecularOutputImage;

  // NRD's permanent pool: internal images that persist across frames, indexed by indexInPool.
  std::vector<rtpt::Image> m_PermanentPoolImages;

  // NRD's transient pool: internal scratch images, indexed by indexInPool.
  std::vector<rtpt::Image> m_TransientPoolImages;

  // Identifier of the single REBLUR_DIFFUSE_SPECULAR denoiser in the instance.
  static constexpr nrd::Identifier kDenoiserIdentifier = 1u;

  // Library-wide description; supplies the SPIR-V binding offsets.
  const nrd::LibraryDesc*  m_LibraryDesc  = nullptr;

  // Instance description: pipelines, pool images, and descriptor limits.
  const nrd::InstanceDesc* m_InstanceDesc = nullptr;

  // Native NRD instance. Null when Initialize has not run or failed.
  nrd::Instance*           m_Instance     = nullptr;

  // REBLUR settings, pushed to NRD on every PrepareFrame.
  nrd::ReblurSettings      m_ReblurSettings {};

  // Camera, resolution, and history settings, rebuilt on every PrepareFrame.
  nrd::CommonSettings      m_CommonSettings {};

  // NRD requires this to advance by one per frame; it restarts at zero with the history.
  uint32_t                 m_FrameIndex = 0;

  // False until one frame has run since the last reset; until then the current matrices stand in for the previous ones.
  bool                     m_HasPreviousMatrices = false;

  // Last frame's world-to-view matrix.
  glm::mat4                m_PreviousViewMatrix { 1.0f };

  // Last frame's view-to-clip matrix.
  glm::mat4                m_PreviousProjectionMatrix { 1.0f };
};

}  // namespace rtpt
