#pragma once

// Role:
// Owns the ray tracing pipeline path: descriptor set layout, pipeline layout,
// ray tracing pipeline, shader binding table, and the per-frame trace dispatch.

#include <cstdint>

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
    VkCommandBuffer                      cmd = VK_NULL_HANDLE;
    const nvsamples::GltfSceneResource*  sceneResource = nullptr;
    const shaderio::GltfSceneInfo*       sceneInfo     = nullptr;
    const nvvk::AccelerationStructure*   topLevelAS    = nullptr;
    nvvk::GBuffer*                       gBuffers      = nullptr;
    uint32_t                             renderedImageIndex = 0;
  };

  explicit PathTracer(const CreateInfo& createInfo);

  void Initialize();
  void Destroy();
  bool IsReady() const;

  // Expose the descriptor pack so Application can keep the texture binding in
  // sync with the scene's texture array the same way the raster path already does.
  nvvk::DescriptorPack&       GetDescriptorPack();
  const nvvk::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:
  void QueryRayTracingProperties();
  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateRayTracingPipeline();
  void CreateShaderBindingTable();
  void UpdateFrameDescriptors(const RenderInput& input);

  nvapp::Application*      m_App       = nullptr;
  nvvk::ResourceAllocator* m_Allocator = nullptr;
  uint32_t                 m_MaxTextureDescriptors = 0;
  uint32_t                 m_FrameNumber = 0;
  uint32_t                 m_MaxBounces  = 0;

  nvvk::DescriptorPack     m_DescPack;
  VkPipelineLayout         m_PipelineLayout = VK_NULL_HANDLE;
  VkPipeline               m_Pipeline       = VK_NULL_HANDLE;
  nvvk::SBTGenerator       m_SbtGenerator;
  nvvk::Buffer             m_SbtBuffer;
  nvvk::SBTGenerator::Regions m_SbtRegions{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

}  // namespace nvsamples
