#pragma once

#include <cstdint>
#include <vector>

#include <glm/mat4x4.hpp>
#include <volk/volk.h>
#include <vulkan/vulkan_core.h>

#include "PathTracing/Common/ResolveMode.h"
#include "DenoiserResources.h"
#include "Shaders/ShaderIo.h"
#include "nvvk/resource_allocator.hpp"
#include "nvvk/resources.hpp"

#include <NRD.h>

namespace nvapp
{
class Application;
}

namespace nvsamples
{

// Owns the native NRD instance, the Vulkan resources that mirror NRD's image
// model, and the small compose step that resolves denoised diffuse/specular
// outputs back into the active renderer's HDR target.
class NrdDenoiser
{
public:
  struct CreateInfo
  {
    nvapp::Application*      app       = nullptr;
    nvvk::ResourceAllocator* allocator = nullptr;
  };

  struct FrameInput
  {
    const shaderio::GltfSceneInfo* sceneInfo           = nullptr;
    VkExtent2D                     viewportSize        = {};
    bool                           historyInvalidated  = false;
    bool                           enableMaterialDemodulation = false;
    const DenoiserSettings*        settings            = nullptr;
  };

  explicit NrdDenoiser(const CreateInfo& createInfo);

  void Initialize();
  void Destroy();
  bool IsReady() const;

  void InvalidateHistory();
  void PrepareFrame(const FrameInput& input, const DenoiserResources& denoiserInputs);
  void Denoise(VkCommandBuffer cmd,
               const DenoiserResources& denoiserInputs,
               VkImageView rawBeautyImageView,
               VkImageView outputImageView,
               DenoiserDebugView debugView,
               VkExtent2D viewportSize);

  const nvvk::Image& GetDiffuseOutputImage() const;
  const nvvk::Image& GetSpecularOutputImage() const;

private:
  struct FrameResources
  {
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet  frameSet       = VK_NULL_HANDLE;
    nvvk::Buffer     constantBuffer;
    uint32_t         constantBufferOffset      = 0;
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
  void DestroyViewportResources(bool deferDestruction);
  void ScheduleImageDestroy(nvvk::Image image);
  void EnsureForViewport(VkExtent2D viewportSize);
  void RecreateViewportResources(VkExtent2D viewportSize);
  nvvk::Image CreateStorageImage(VkExtent2D viewportSize, VkFormat format, const char* debugName) const;
  void ApplyDenoiserSettings(const DenoiserSettings& settings);
  void UpdateCommonSettings(const FrameInput& input);
  FrameResources& GetCurrentFrameResources();
  void BeginFrame(FrameResources& frameResources);
  void UpdateFrameSet(FrameResources& frameResources);
  VkDescriptorSet AllocateDescriptorSet(FrameResources& frameResources, VkDescriptorSetLayout layout);
  uint32_t UploadConstantData(FrameResources& frameResources, const void* constantData, uint32_t constantDataSize, bool reusePreviousData);
  void UpdateResourceSet(VkDescriptorSet resourceSet, const nrd::DispatchDesc& dispatchDesc, const DenoiserResources& denoiserInputs);
  void DispatchNrd(VkCommandBuffer cmd, FrameResources& frameResources, const DenoiserResources& denoiserInputs);
  void ComposeDenoisedResult(VkCommandBuffer cmd,
                             FrameResources& frameResources,
                             const DenoiserResources& denoiserInputs,
                             VkImageView rawBeautyImageView,
                             VkImageView outputImageView,
                             DenoiserDebugView debugView,
                             VkExtent2D viewportSize);
  const nvvk::Image& ResolveDispatchImage(nrd::ResourceType resourceType, uint16_t poolIndex, const DenoiserResources& denoiserInputs) const;
  void TransitionImageToGeneral(VkCommandBuffer cmd, nvvk::Image& image, VkPipelineStageFlags2 dstStageMask) const;
  void InsertComputeBarrier(VkCommandBuffer cmd) const;

  static VkDescriptorType ToVkDescriptorType(nrd::DescriptorType descriptorType);
  static VkFormat ToVkFormat(nrd::Format format);
  static void     CopyMatrix(glm::mat4 matrix, float (&destination)[16]);

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  VkExtent2D               m_ViewportSize{};
  bool                     m_HistoryInvalidated = true;
  bool                     m_EnableMaterialDemodulation = false;

  VkSampler m_NearestSampler = VK_NULL_HANDLE;
  VkSampler m_LinearSampler  = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_ResourceSetLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_FrameSetLayout    = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_ComposeSetLayout  = VK_NULL_HANDLE;
  VkPipelineLayout      m_PipelineLayout    = VK_NULL_HANDLE;
  VkPipelineLayout      m_ComposePipelineLayout = VK_NULL_HANDLE;
  std::vector<VkPipeline> m_Pipelines;
  VkPipeline              m_ComposePipeline = VK_NULL_HANDLE;
  std::vector<FrameResources> m_FrameResources;
  uint32_t m_ConstantBufferAlignment = 256;

  nvvk::Image m_DiffuseOutputImage;
  nvvk::Image m_SpecularOutputImage;
  std::vector<nvvk::Image> m_PermanentPoolImages;
  std::vector<nvvk::Image> m_TransientPoolImages;

  static constexpr nrd::Identifier kDenoiserIdentifier = 1u;

  const nrd::LibraryDesc*  m_LibraryDesc  = nullptr;
  const nrd::InstanceDesc* m_InstanceDesc = nullptr;
  nrd::Instance*           m_Instance     = nullptr;
  nrd::ReblurSettings      m_ReblurSettings{};
  nrd::CommonSettings      m_CommonSettings{};
  uint32_t                 m_FrameIndex = 0;
  bool                     m_HasPreviousMatrices = false;
  glm::mat4                m_PreviousViewMatrix{1.0f};
  glm::mat4                m_PreviousProjectionMatrix{1.0f};
};

}  // namespace nvsamples
