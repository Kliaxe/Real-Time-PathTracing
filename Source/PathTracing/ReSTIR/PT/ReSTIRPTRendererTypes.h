#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan_core.h>

#include "Common/GltfUtils.hpp"
#include "PathTracing/ReSTIR/PT/ReSTIRPTParameterContext.h"
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

// CPU-side construction parameters for the ReSTIR PT renderer.
// These are stable lifetime dependencies supplied by Application.
struct ReSTIRPTRendererCreateInfo
{
  nvapp::Application*      app                   = nullptr;
  nvvk::ResourceAllocator* allocator             = nullptr;
  uint32_t                 maxTextureDescriptors = 0;
};

// One-frame render input borrowed from Application and SceneRuntime.
// Nothing here is owned by the renderer; it only records commands against it.
struct ReSTIRPTRenderInput
{
  VkCommandBuffer                     cmd                = VK_NULL_HANDLE;
  const nvsamples::GltfSceneResource* sceneResource      = nullptr;
  const shaderio::GltfSceneInfo*      sceneInfo          = nullptr;
  const nvvk::AccelerationStructure*  topLevelAS         = nullptr;
  nvvk::GBuffer*                      gBuffers           = nullptr;
  uint32_t                            renderedImageIndex = 0;
};

// Compact CPU-side key answering "can the history from last frame still be
// trusted?". Accumulation averages pixels visually, so it depends on the camera
// as well as the lighting environment; any change here means the average is of
// two different images and must restart.
struct ReSTIRPTAccumulationSignature
{
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

inline ReSTIRPTAccumulationSignature MakeReSTIRPTAccumulationSignature(const shaderio::GltfSceneInfo& sceneInfo,
                                                                      VkDeviceAddress                topLevelAsAddress,
                                                                      VkExtent2D                     viewportSize)
{
  // Kept explicit so adding or removing a history dependency is easy to review.
  ReSTIRPTAccumulationSignature signature{};
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

// Answers the narrower question "can NRD's temporal history still be trusted?".
// Deliberately smaller than the accumulation signature: NRD reprojects with the
// motion vectors this renderer writes, so a camera move is not a reason to drop
// its history - only a change to what is being lit or to the buffer sizes is.
struct ReSTIRPTDenoiserHistorySignature
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

inline ReSTIRPTDenoiserHistorySignature MakeReSTIRPTDenoiserHistorySignature(const shaderio::GltfSceneInfo& sceneInfo,
                                                                            VkDeviceAddress topLevelAsAddress,
                                                                            VkExtent2D      viewportSize)
{
  ReSTIRPTDenoiserHistorySignature signature{};
  signature.useSky                  = sceneInfo.useSky;
  signature.useHdrEnv               = sceneInfo.useHdrEnv;
  signature.environmentTextureIndex = sceneInfo.environmentTextureIndex;
  signature.backgroundColor         = sceneInfo.backgroundColor;
  signature.skySimpleParam          = sceneInfo.skySimpleParam;
  signature.topLevelAsAddress       = topLevelAsAddress;
  signature.viewportSize            = viewportSize;
  return signature;
}

inline bool IsReSTIRPTTemporalResamplingEnabled(ReSTIRPTResamplingMode resamplingMode)
{
  return resamplingMode == ReSTIRPTResamplingMode::eTemporal || resamplingMode == ReSTIRPTResamplingMode::eTemporalAndSpatial;
}

inline bool IsReSTIRPTSpatialResamplingEnabled(ReSTIRPTResamplingMode resamplingMode)
{
  return resamplingMode == ReSTIRPTResamplingMode::eSpatial || resamplingMode == ReSTIRPTResamplingMode::eTemporalAndSpatial;
}

inline uint32_t GetReSTIRPTFrameSetIndex(uint32_t frameCycleIndex, size_t setCount)
{
  // Descriptor/parameter buffers are allocated per frame set; guard empty packs on shutdown paths.
  if(setCount == 0)
  {
    return 0;
  }

  return std::min(frameCycleIndex, uint32_t(setCount - 1));
}

}  // namespace nvsamples
