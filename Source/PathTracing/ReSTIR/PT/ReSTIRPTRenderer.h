#pragma once

#include <array>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_core.h>

#include "PathTracing/ReSTIR/Common/ReSTIRContext.h"
#include "PathTracing/ReSTIR/Common/ReSTIRResources.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTSettings.h"
#include "Common/GltfUtils.hpp"
#include "Shaders/ShaderIo.h"
#include "nvvk/descriptors.hpp"
#include "nvvk/gbuffers.hpp"
#include "nvvk/resource_allocator.hpp"
#include "nvvk/sbt_generator.hpp"

namespace nvapp
{
class Application;
}

namespace nvsamples
{

// CPU-side owner of the thesis ReSTIR PT pipeline. It builds the Vulkan
// objects, owns the history invalidation contract, and records the four-pass
// frame sequence.
class ReSTIRPTRenderer
{
public:
  struct CreateInfo
  {
    nvapp::Application*      app                   = nullptr;
    nvvk::ResourceAllocator* allocator             = nullptr;
    uint32_t                 maxTextureDescriptors = 0;
  };

  struct RenderInput
  {
    // The renderer borrows scene/runtime state for one frame. Ownership remains
    // in Application and SceneRuntime.
    VkCommandBuffer                     cmd                = VK_NULL_HANDLE;
    const nvsamples::GltfSceneResource* sceneResource      = nullptr;
    const shaderio::GltfSceneInfo*      sceneInfo          = nullptr;
    const nvvk::AccelerationStructure*  topLevelAS         = nullptr;
    nvvk::GBuffer*                      gBuffers           = nullptr;
    uint32_t                            renderedImageIndex = 0;
  };

  explicit ReSTIRPTRenderer(const CreateInfo& createInfo);

  void Initialize();
  void Destroy();
  bool IsReady() const;

  ReSTIRPTSettings&       GetSettings();
  const ReSTIRPTSettings& GetSettings() const;
  uint32_t                GetAccumulatedFrameCount() const;
  uint32_t                GetPipelineBounceLimit() const;
  void                    InvalidateHistory();

  nvvk::DescriptorPack&       GetDescriptorPack();
  const nvvk::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:
  // Tracks the camera/scene state that matters for accumulation resets. ReSTIR
  // reuse itself is allowed to survive normal camera motion when accumulation
  // is disabled.
  struct HistorySignature
  {
    glm::mat4                     viewProjMatrix{};
    glm::mat4                     projInvMatrix{};
    glm::mat4                     viewInvMatrix{};
    glm::vec3                     cameraPosition{};
    int                           useSky                  = 0;
    int                           useHdrEnv               = 0;
    int                           environmentTextureIndex = -1;
    int                           pad0                    = 0;
    glm::vec3                     backgroundColor{};
    int                           pad1 = 0;
    shaderio::SkySimpleParameters skySimpleParam{};
    VkDeviceAddress               topLevelAsAddress = 0;
    VkExtent2D                    viewportSize{};
  };

  struct RayTracingPassState
  {
    // Each ray tracing pass owns a full pipeline plus its matching shader
    // binding table because Vulkan binds those together at dispatch time.
    VkPipeline                  pipeline = VK_NULL_HANDLE;
    nvvk::SBTGenerator          sbtGenerator;
    nvvk::Buffer                sbtBuffer;
    nvvk::SBTGenerator::Regions sbtRegions{};
  };

  enum class ComputePass : uint32_t
  {
    eTemporal = 0,
    eSpatial,
    eCount,
  };

  void QueryRayTracingProperties();
  // The descriptor layout is shared by all ReSTIR PT passes so the CPU/GPU
  // contract stays stable while only the active pipeline changes.
  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateInitialSamplingPipeline();
  void CreateFinalShadingPipeline();
  void CreateComputePipelines();
  uint32_t BuildFrameFlags(bool enableTemporal, bool enableSpatial) const;
  void CreateRayTracingPass(const VkShaderModuleCreateInfo& shaderCode, RayTracingPassState& passState, const char* debugName);
  void DestroyRayTracingPass(RayTracingPassState& passState);
  void UpdateFrameDescriptors(const RenderInput& input);
  // Storage images are transitioned once per frame before any pass writes them.
  void TransitionStorageImages(VkCommandBuffer cmd, const RenderInput& input);
  void RunInitialSamplingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunTemporalPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunSpatialPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void RunFinalShadingPass(const RenderInput& input, const shaderio::ReSTIRPTPushConstant& pushConstant);
  void DispatchComputePass(VkCommandBuffer cmd, VkPipeline pipeline, const shaderio::ReSTIRPTPushConstant& pushConstant,
                           VkExtent2D viewportSize);
  VkPipeline CreateComputePipeline(const VkShaderModuleCreateInfo& shaderCode, const char* debugName) const;
  HistorySignature MakeHistorySignature(const RenderInput& input, VkExtent2D viewportSize) const;

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  uint32_t                 m_MaxTextureDescriptors = 0;
  uint32_t                 m_MaxBounceLimit        = 0;
  uint32_t                 m_PipelineBounceLimit   = 0;
  uint32_t                 m_AccumulatedFrames     = 0;
  bool                     m_HistoryInvalidated    = true;
  bool                     m_HasHistorySignature   = false;

  ReSTIRPTSettings         m_Settings{};
  // Stored so render-time signature comparison can reset accumulated output
  // only when the accumulation contract changes.
  HistorySignature         m_LastHistorySignature{};
  ReSTIRContext            m_Context;
  ReSTIRResources          m_Resources;

  nvvk::DescriptorPack        m_DescPack;
  VkPipelineLayout            m_PipelineLayout = VK_NULL_HANDLE;
  RayTracingPassState         m_InitialSamplingPass;
  RayTracingPassState         m_FinalShadingPass;
  std::array<VkPipeline, static_cast<size_t>(ComputePass::eCount)> m_ComputePipelines{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples
