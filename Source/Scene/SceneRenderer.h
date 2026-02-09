#pragma once

// Role:
// Encapsulates raster scene drawing (including optional sky/HDRI background draw).

#include <memory>

#include <glm/glm.hpp>
#include <vulkan/vulkan_core.h>

#include "Common/GltfUtils.hpp"
#include "nvshaders_host/sky.hpp"
#include "nvvk/descriptors.hpp"
#include "nvvk/gbuffers.hpp"
#include "nvvk/graphics_pipeline.hpp"

namespace nvutils
{
class CameraManipulator;
}

namespace nvsamples
{

class SceneRenderer
{
public:
  struct RenderInput
  {
    VkCommandBuffer                               cmd = VK_NULL_HANDLE;
    const nvsamples::GltfSceneResource*          sceneResource = nullptr;
    const shaderio::GltfSceneInfo*               sceneInfo     = nullptr;
    const glm::vec2*                             metallicRoughnessOverride = nullptr;
    const VkExtent2D*                            viewportSize  = nullptr;
    std::shared_ptr<nvutils::CameraManipulator>  cameraManip;
    nvshaders::SkySimple*                        skySimple = nullptr;
    nvvk::GBuffer*                               gBuffers = nullptr;
    nvvk::GraphicsPipelineState*                 dynamicPipeline = nullptr;
    nvvk::DescriptorPack*                        descPack = nullptr;
    VkPipelineLayout                             graphicsPipelineLayout = VK_NULL_HANDLE;
    VkShaderEXT                                  vertexShader = VK_NULL_HANDLE;
    VkShaderEXT                                  fragmentShader = VK_NULL_HANDLE;
    uint32_t                                     renderedImageIndex = 0;
  };

  void Render(const RenderInput& input) const;
};

}  // namespace nvsamples
