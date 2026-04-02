#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_core.h>

#include "Common/GltfUtils.hpp"
#include "PathTracing/ReSTIR/Common/ReSTIRSettings.h"
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

// Shared CPU-side contract for the thesis ReSTIR-family renderers.
struct ReSTIRRendererCreateInfo
{
  nvapp::Application*      app                   = nullptr;
  nvvk::ResourceAllocator* allocator             = nullptr;
  uint32_t                 maxTextureDescriptors = 0;
};

// One-frame render input borrowed from Application and SceneRuntime.
struct ReSTIRRenderInput
{
  VkCommandBuffer                     cmd                = VK_NULL_HANDLE;
  const nvsamples::GltfSceneResource* sceneResource      = nullptr;
  const shaderio::GltfSceneInfo*      sceneInfo          = nullptr;
  const nvvk::AccelerationStructure*  topLevelAS         = nullptr;
  nvvk::GBuffer*                      gBuffers           = nullptr;
  uint32_t                            renderedImageIndex = 0;
};

struct ReSTIRHistorySignature
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

struct ReSTIRDenoiserHistorySignature
{
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

inline ReSTIRHistorySignature MakeReSTIRHistorySignature(const shaderio::GltfSceneInfo& sceneInfo,
                                                         VkDeviceAddress topLevelAsAddress,
                                                         VkExtent2D viewportSize)
{
  ReSTIRHistorySignature signature{};
  signature.viewProjMatrix          = sceneInfo.viewProjMatrix;
  signature.projInvMatrix           = sceneInfo.projInvMatrix;
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

inline ReSTIRDenoiserHistorySignature MakeReSTIRDenoiserHistorySignature(const shaderio::GltfSceneInfo& sceneInfo,
                                                                         VkDeviceAddress topLevelAsAddress,
                                                                         VkExtent2D viewportSize)
{
  ReSTIRDenoiserHistorySignature signature{};
  signature.useSky                  = sceneInfo.useSky;
  signature.useHdrEnv               = sceneInfo.useHdrEnv;
  signature.environmentTextureIndex = sceneInfo.environmentTextureIndex;
  signature.backgroundColor         = sceneInfo.backgroundColor;
  signature.skySimpleParam          = sceneInfo.skySimpleParam;
  signature.topLevelAsAddress       = topLevelAsAddress;
  signature.viewportSize            = viewportSize;
  return signature;
}

inline bool IsReSTIRTemporalResamplingEnabled(ReSTIRResamplingMode resamplingMode)
{
  return resamplingMode == ReSTIRResamplingMode::eTemporal || resamplingMode == ReSTIRResamplingMode::eTemporalAndSpatial;
}

inline bool IsReSTIRSpatialResamplingEnabled(ReSTIRResamplingMode resamplingMode)
{
  return resamplingMode == ReSTIRResamplingMode::eSpatial || resamplingMode == ReSTIRResamplingMode::eTemporalAndSpatial;
}

inline uint32_t GetReSTIRFrameSetIndex(uint32_t frameCycleIndex, size_t setCount)
{
  if(setCount == 0)
  {
    return 0;
  }

  return std::min(frameCycleIndex, uint32_t(setCount - 1));
}

}  // namespace nvsamples
