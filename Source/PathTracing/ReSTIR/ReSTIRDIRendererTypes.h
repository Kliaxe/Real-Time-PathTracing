#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_core.h>

#include "Common/GltfUtils.hpp"
#include "PathTracing/ReSTIR/ReSTIRDISettings.h"
#include "Shaders/ShaderIo.h"
#include "nvvk/gbuffers.hpp"
#include "nvvk/resources.hpp"

namespace nvapp
{
class Application;
}

namespace nvvk
{
class ResourceAllocator;
}

namespace nvsamples
{

// CPU-side construction parameters for the ReSTIR DI renderer.
// These are stable lifetime dependencies supplied by Application.
struct ReSTIRDIRendererCreateInfo
{
  nvapp::Application*      app                   = nullptr;
  nvvk::ResourceAllocator* allocator             = nullptr;
  uint32_t                 maxTextureDescriptors = 0;
};

// One-frame render input borrowed from Application and SceneRuntime.
// Nothing here is owned by ReSTIR; the renderer only records commands against it.
struct ReSTIRDIRenderInput
{
  VkCommandBuffer                     cmd                = VK_NULL_HANDLE;
  const nvsamples::GltfSceneResource* sceneResource      = nullptr;
  const shaderio::GltfSceneInfo*      sceneInfo          = nullptr;
  const nvvk::AccelerationStructure*  topLevelAS         = nullptr;
  nvvk::GBuffer*                      gBuffers           = nullptr;
  uint32_t                            renderedImageIndex = 0;
};

struct ReSTIRDIAccumulationSignature
{
  // Accumulation depends on camera and background state because old pixels are averaged visually.
  glm::mat4                     viewProjMatrix{};
  glm::mat4                     viewProjInvMatrix{};
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

struct ReSTIRDIDenoiserHistorySignature
{
  // NRD history depends on lighting/background state, but not on camera matrices directly.
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

inline ReSTIRDIAccumulationSignature MakeReSTIRDIAccumulationSignature(const shaderio::GltfSceneInfo& sceneInfo,
                                                                       VkDeviceAddress topLevelAsAddress,
                                                                       VkExtent2D viewportSize)
{
  // Keep this builder explicit so adding/removing a history dependency is easy to review.
  ReSTIRDIAccumulationSignature signature{};
  signature.viewProjMatrix          = sceneInfo.viewProjMatrix;
  signature.viewProjInvMatrix       = sceneInfo.viewProjInvMatrix;
  signature.viewInvMatrix           = sceneInfo.viewInvMatrix;
  signature.cameraPosition          = sceneInfo.cameraPosition;
  signature.useSky                  = sceneInfo.useSky;
  signature.useHdrEnv               = sceneInfo.useHdrEnv;
  signature.environmentTextureIndex = sceneInfo.environmentTextureIndex;
  signature.backgroundColor         = sceneInfo.backgroundColor;
  signature.skySimpleParam          = sceneInfo.skySimpleParam;
  signature.topLevelAsAddress       = topLevelAsAddress;
  signature.viewportSize            = viewportSize;
  return signature;
}

inline ReSTIRDIDenoiserHistorySignature MakeReSTIRDIDenoiserHistorySignature(const shaderio::GltfSceneInfo& sceneInfo,
                                                                             VkDeviceAddress topLevelAsAddress,
                                                                             VkExtent2D viewportSize)
{
  // This is intentionally smaller than the accumulation signature.
  ReSTIRDIDenoiserHistorySignature signature{};
  signature.useSky                  = sceneInfo.useSky;
  signature.useHdrEnv               = sceneInfo.useHdrEnv;
  signature.environmentTextureIndex = sceneInfo.environmentTextureIndex;
  signature.backgroundColor         = sceneInfo.backgroundColor;
  signature.skySimpleParam          = sceneInfo.skySimpleParam;
  signature.topLevelAsAddress       = topLevelAsAddress;
  signature.viewportSize            = viewportSize;
  return signature;
}

inline bool IsReSTIRDITemporalResamplingEnabled(ReSTIRDIResamplingMode resamplingMode)
{
  return resamplingMode == ReSTIRDIResamplingMode::eTemporal || resamplingMode == ReSTIRDIResamplingMode::eTemporalAndSpatial;
}

inline bool IsReSTIRDISpatialResamplingEnabled(ReSTIRDIResamplingMode resamplingMode)
{
  return resamplingMode == ReSTIRDIResamplingMode::eSpatial || resamplingMode == ReSTIRDIResamplingMode::eTemporalAndSpatial;
}

inline uint32_t GetReSTIRDIFrameSetIndex(uint32_t frameCycleIndex, size_t setCount)
{
  // Descriptor/parameter buffers are allocated per frame set, but guard empty packs for shutdown paths.
  if(setCount == 0)
  {
    return 0;
  }

  return std::min(frameCycleIndex, uint32_t(setCount - 1));
}

}  // namespace nvsamples
