#pragma once

#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_core.h>

#include "Common/GltfUtils.hpp"
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

class PathTracer
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
    VkCommandBuffer                     cmd                = VK_NULL_HANDLE;
    const nvsamples::GltfSceneResource* sceneResource      = nullptr;
    const shaderio::GltfSceneInfo*      sceneInfo          = nullptr;
    const nvvk::AccelerationStructure*  topLevelAS         = nullptr;
    nvvk::GBuffer*                      gBuffers           = nullptr;
    uint32_t                            renderedImageIndex = 0;
  };

  struct Settings
  {
    bool     accumulate = false;
    uint32_t maxBounces = 8;
  };

  explicit PathTracer(const CreateInfo& createInfo);

  void Initialize();
  void Destroy();
  bool IsReady() const;

  Settings&       GetSettings();
  const Settings& GetSettings() const;
  uint32_t        GetAccumulatedFrameCount() const;
  uint32_t        GetPipelineBounceLimit() const;
  void            InvalidateAccumulation();

  nvvk::DescriptorPack&       GetDescriptorPack();
  const nvvk::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:
  struct AccumulationSignature
  {
    glm::mat4                   viewProjMatrix{};
    glm::mat4                   projInvMatrix{};
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

  void QueryRayTracingProperties();
  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateRayTracingPipeline();
  void CreateShaderBindingTable();
  void UpdateFrameDescriptors(const RenderInput& input);
  void CreateOrResizeAccumulationImage(VkExtent2D size);
  void DestroyAccumulationImage();
  void ScheduleAccumulationImageDestroy(nvvk::Image image);
  AccumulationSignature MakeAccumulationSignature(const RenderInput& input, VkExtent2D size) const;

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  uint32_t                 m_MaxTextureDescriptors = 0;
  uint32_t                 m_RngFrameNumber        = 0;
  uint32_t                 m_MaxBounceLimit        = 0;
  uint32_t                 m_PipelineBounceLimit   = 0;
  uint32_t                 m_AccumulatedFrames     = 0;
  bool                     m_AccumulationInvalidated = true;
  bool                     m_HasAccumulationSignature = false;
  Settings                 m_Settings{};
  AccumulationSignature    m_LastAccumulationSignature{};

  nvvk::DescriptorPack        m_DescPack;
  VkPipelineLayout            m_PipelineLayout = VK_NULL_HANDLE;
  VkPipeline                  m_Pipeline       = VK_NULL_HANDLE;
  nvvk::SBTGenerator          m_SbtGenerator;
  nvvk::Buffer                m_SbtBuffer;
  nvvk::Image                 m_AccumulationImage;
  nvvk::SBTGenerator::Regions m_SbtRegions{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples









