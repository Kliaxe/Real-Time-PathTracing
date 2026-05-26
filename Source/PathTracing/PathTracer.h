#pragma once

#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_core.h>

#include "Common/GltfUtils.hpp"
#include "PathTracing/Common/ResolveMode.h"
#include "Denoising/DenoiserResources.h"
#include "Denoising/NrdDenoiser.h"
#include "Shaders/ShaderIo.h"
#include "nvvk/descriptors.hpp"
#include "nvvk/gbuffers.hpp"
#include "nvvk/resource_allocator.hpp"
#include "nvvk/resources.hpp"
#include "nvvk/sbt_generator.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

// Ground-truth and direct-sampling baseline renderer. It owns the ray tracing
// pipeline, accumulation target, and NRD resources used by the baseline modes.
// CPU responsibility: prepare Vulkan state and record one ray tracing dispatch.
// Shader responsibility: trace paths, accumulate radiance, and write NRD signals.
class PathTracer
{
public:
  struct CreateInfo
  {
    // Lifetime dependencies owned by Application.
    nvapp::Application*      app                   = nullptr;
    nvvk::ResourceAllocator* allocator             = nullptr;
    uint32_t                 maxTextureDescriptors = 0;
  };

  struct RenderInput
  {
    // One-frame borrowed state. PathTracer records commands but owns none of these objects.
    VkCommandBuffer                     cmd                = VK_NULL_HANDLE;
    const nvsamples::GltfSceneResource* sceneResource      = nullptr;
    const shaderio::GltfSceneInfo*      sceneInfo          = nullptr;
    const nvvk::AccelerationStructure*  topLevelAS         = nullptr;
    nvvk::GBuffer*                      gBuffers           = nullptr;
    uint32_t                            renderedImageIndex = 0;
  };

  struct Settings
  {
    // Resolve mode decides whether the noisy image is raw, accumulated, or denoised.
    RenderResolveMode resolveMode       = RenderResolveMode::eOff;
    DenoiserDebugView denoiserDebugView = DenoiserDebugView::eFinal;
    DenoiserSettings  denoiserSettings{};
    // Clamped against Vulkan ray recursion support before reaching the shader.
    uint32_t          maxBounces        = 8;
  };

  explicit PathTracer(const CreateInfo& createInfo);

  void Initialize();
  void Destroy();
  bool IsReady() const;

  Settings&       GetSettings();
  const Settings& GetSettings() const;
  uint32_t        GetAccumulatedFrameCount() const;
  uint32_t        GetPipelineBounceLimit() const;
  void            InvalidateHistory();

  nvvk::DescriptorPack&       GetDescriptorPack();
  const nvvk::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:
  struct AccumulationSignature
  {
    // Accumulation depends on camera and lighting state because old pixels are averaged visually.
    glm::mat4                   viewProjMatrix{};
    glm::mat4                   viewProjInvMatrix{};
    glm::mat4                   viewInvMatrix{};
    glm::vec3                   cameraPosition{};
    int                         useSky                  = 0;
    int                         useHdrEnv               = 0;
    int                         environmentTextureIndex = -1;
    int                         _pad0                   = 0;
    glm::vec3                   backgroundColor{};
    int                         _pad1 = 0;
    shaderio::SkySimpleParameters skySimpleParam{};
    VkDeviceAddress             topLevelAsAddress = 0;
    VkExtent2D                  viewportSize{};
  };

  struct DenoiserSignature
  {
    // NRD history depends on lighting/background state, but not on camera matrices directly.
    int                         useSky                  = 0;
    int                         useHdrEnv               = 0;
    int                         environmentTextureIndex = -1;
    int                         _pad0                   = 0;
    glm::vec3                   backgroundColor{};
    int                         _pad1 = 0;
    shaderio::SkySimpleParameters skySimpleParam{};
    VkDeviceAddress             topLevelAsAddress = 0;
    VkExtent2D                  viewportSize{};
  };

  struct FrameState
  {
    // Snapshot of decisions that must remain consistent for one recorded frame.
    VkExtent2D            viewportSize{};
    AccumulationSignature accumulationSignature{};
    DenoiserSignature     denoiserSignature{};
    bool                  finalAccumulationEnabled = false;
    bool                  denoiserHistoryInvalidated = false;
  };

  void QueryRayTracingProperties();
  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateRayTracingPipeline();
  void CreateShaderBindingTable();
  bool CanRender(const RenderInput& input) const;
  void EnsureViewportResources(VkExtent2D viewportSize);
  FrameState BeginPathTraceFrame(const RenderInput& input, VkExtent2D viewportSize);
  void PrepareDenoiser(const RenderInput& input, const FrameState& frameState);
  void PrepareStorageImages(const RenderInput& input);
  shaderio::PathTracePushConstant BuildPushConstant(const RenderInput& input, const FrameState& frameState);
  void RecordPathTracePass(const RenderInput& input, const shaderio::PathTracePushConstant& pushConstant);
  void RunDenoiserIfNeeded(const RenderInput& input, const FrameState& frameState);
  void FinishFrame(const FrameState& frameState);
  void UpdateFrameDescriptors(const RenderInput& input);
  void CreateOrResizeAccumulationImage(VkExtent2D size);
  void DestroyAccumulationImage();
  void ScheduleAccumulationImageDestroy(nvvk::Image image);
  void TransitionStorageImageForWrite(VkCommandBuffer cmd, nvvk::Image& image, VkPipelineStageFlags2 dstStageMask);
  AccumulationSignature MakeAccumulationSignature(const RenderInput& input, VkExtent2D size) const;
  DenoiserSignature MakeDenoiserSignature(const RenderInput& input, VkExtent2D size) const;

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  uint32_t                 m_MaxTextureDescriptors = 0;
  // Advances every rendered frame to decorrelate random samples, even when accumulation is off.
  uint32_t                 m_RngFrameNumber        = 0;
  // Device limit comes from Vulkan; pipeline limit is the depth this renderer requested.
  uint32_t                 m_DeviceBounceLimit     = 0;
  uint32_t                 m_PipelineBounceLimit   = 0;
  // Accumulation is presentation/reference history, separate from NRD history.
  uint32_t                 m_AccumulatedFrames     = 0;
  bool                     m_AccumulationInvalidated = true;
  bool                     m_HasAccumulationSignature = false;
  bool                     m_HasDenoiserSignature = false;
  Settings                 m_Settings{};
  // Signatures are compact CPU-side keys for "can old history still be trusted?"
  AccumulationSignature    m_LastAccumulationSignature{};
  DenoiserSignature        m_LastDenoiserSignature{};

  nvvk::DescriptorPack        m_DescPack;
  VkPipelineLayout            m_PipelineLayout = VK_NULL_HANDLE;
  VkPipeline                  m_Pipeline       = VK_NULL_HANDLE;
  // Vulkan ray tracing needs an SBT that maps TraceRay indices to shader groups.
  nvvk::SBTGenerator          m_SbtGenerator;
  nvvk::Buffer                m_SbtBuffer;
  nvvk::Image                 m_AccumulationImage;
  DenoiserResources  m_DenoiserResources;
  NrdDenoiser        m_NrdDenoiser;
  nvvk::SBTGenerator::Regions m_SbtRegions{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples
