#pragma once

#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <volk.h>

#include "Framework/Vulkan/Descriptors.h"
#include "Framework/Vulkan/GpuProfiler.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "Rendering/RenderTargetView.h"
#include "Rendering/SkyRenderer.h"
#include "Scene/SceneGpuResources.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

// RasterRenderer
// Rasterizer preview: the third render mode next to the reference path tracer and ReSTIR PT, shaped the same way so Application treats all three alike.
// It owns the scene texture descriptor set, the pipeline layout, the mesh and background graphics pipelines built from Rasterizer.hlsl, and the procedural sky pass.
// CPU responsibility: prepare Vulkan state and record one raster pass. Shader responsibility: shade the meshes and sample the environment.

class RasterRenderer
{
public:

  // CreateInfo
  // Lifetime dependencies owned by Application. RasterRenderer borrows the device and must be destroyed first.

  struct CreateInfo
  {
    // Logical device the descriptor set, layout, and pipelines are created on.
    rtpt::VulkanDevice* device                = nullptr;
    // Size of the bindless texture arrays in the descriptor layout.
    uint32_t            maxTextureDescriptors = 0;
    // Attachment formats the graphics pipelines are built for; they must match the targets Render receives.
    VkFormat            colorFormat           = VK_FORMAT_UNDEFINED;
    VkFormat            depthFormat           = VK_FORMAT_UNDEFINED;
  };

  // RenderInput
  // One-frame borrowed state. RasterRenderer records commands but owns none of these objects.

  struct RenderInput
  {
    // Command buffer the pass is recorded into.
    VkCommandBuffer                cmd           = VK_NULL_HANDLE;
    // Scene geometry, instances, and the device address of the scene uniform.
    const rtpt::GltfSceneResource* sceneResource = nullptr;
    // CPU copy of the scene uniform, used to choose the background.
    const shaderio::GltfSceneInfo* sceneInfo     = nullptr;
    // Camera matrices, used by the sky pass.
    glm::mat4                      viewMatrix { 1.0F };
    glm::mat4                      projectionMatrix { 1.0F };
    // Color output; left in GENERAL layout when the pass finishes. Its extent is the render area.
    rtpt::RenderTargetView         colorTarget {};
    // Depth attachment, cleared every frame.
    rtpt::RenderTargetView         depthTarget {};
    // Slot of the frame being recorded; the profiler keeps its queries per slot.
    rtpt::FrameSlot                frameSlot {};
    // Optional. Receives one timestamp scope around the sky and draw pass; null records no timing.
    rtpt::GpuProfiler*             profiler = nullptr;
  };

  explicit RasterRenderer(const CreateInfo& createInfo);
  ~RasterRenderer();

  void Initialize();
  void Destroy();
  bool IsReady() const;

  rtpt::DescriptorPack&       GetDescriptorPack();
  const rtpt::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:

  // Creation
  // Called once from Initialize, in an order where each step only depends on the ones before it.

  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateGraphicsPipelines();

  // Builds one variant of the graphics pipeline; depthEnabled toggles depth test and depth write together.
  VkResult CreatePipelineVariant(VkShaderModule vertex, VkShaderModule fragment, bool depthEnabled, VkPipeline& pipeline) const;

  // Lifetime dependencies, borrowed through CreateInfo.

  // Logical device the pipeline objects belong to.
  rtpt::VulkanDevice* m_Device                = nullptr;
  // Size of the bindless texture arrays in the descriptor layout.
  uint32_t            m_MaxTextureDescriptors = 0;
  // Attachment formats the pipelines were built for.
  VkFormat            m_ColorFormat           = VK_FORMAT_UNDEFINED;
  VkFormat            m_DepthFormat           = VK_FORMAT_UNDEFINED;

  // Vulkan state

  // One descriptor set holding the scene textures, updated by Application on scene reloads.
  rtpt::DescriptorPack m_DescPack;
  // Layout shared by both graphics pipelines, with a push constant visible to every graphics stage.
  VkPipelineLayout     m_PipelineLayout     = VK_NULL_HANDLE;
  // Depth-tested pipeline for scene meshes.
  VkPipeline           m_MeshPipeline       = VK_NULL_HANDLE;
  // Depth-disabled pipeline for the fullscreen HDRI background draw recorded before the meshes.
  VkPipeline           m_BackgroundPipeline = VK_NULL_HANDLE;
  // Compute pass that paints the procedural sky into the color target when the scene's sky is enabled.
  rtpt::SkyRenderer    m_Sky;
};

}  // namespace rtpt
