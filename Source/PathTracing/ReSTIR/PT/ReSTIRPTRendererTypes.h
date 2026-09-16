#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <volk.h>

#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuProfiler.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "Rendering/RenderTargetView.h"
#include "Scene/SceneGpuResources.h"
#include "PathTracing/ReSTIR/PT/ReSTIRPTParameterContext.h"
#include "Shaders/ShaderIo.h"
#include "Sampling/SpatiotemporalBlueNoise.h"

namespace rtpt
{

// ReSTIRPTRendererCreateInfo
// CPU-side construction parameters for the ReSTIR PT renderer.
// These are stable lifetime dependencies supplied by Application; the renderer borrows them and must be destroyed before they are.

struct ReSTIRPTRendererCreateInfo
{
  // Logical device and its ray tracing support queries.
  rtpt::VulkanDevice*      device                = nullptr;
  // Allocates every buffer and image the renderer owns.
  rtpt::ResourceAllocator* resources             = nullptr;
  // Optional. Names Vulkan objects for debugging tools when present.
  const rtpt::Diagnostics* diagnostics           = nullptr;
  // Shared immutable sampling volume, also used by the reference tracer.
  const SpatiotemporalBlueNoise* blueNoise         = nullptr;
  // Frames that can be in flight at once. One descriptor set and one parameter buffer are allocated per slot.
  uint32_t                 frameSlotCount        = 0;
  // Size of the bindless texture arrays in the descriptor layout.
  uint32_t                 maxTextureDescriptors = 0;
};

// ReSTIRPTRenderInput
// One-frame render input borrowed from Application and SceneRuntime.
// Nothing here is owned by the renderer; it only records commands against it.

struct ReSTIRPTRenderInput
{
  // Command buffer this frame's passes are recorded into.
  VkCommandBuffer                     cmd                = VK_NULL_HANDLE;
  // GPU scene buffers; the scene info address is passed to the shaders through push constants.
  const rtpt::GltfSceneResource*      sceneResource      = nullptr;
  // CPU copy of the scene info, used for history signatures and NRD camera state.
  const shaderio::GltfSceneInfo*      sceneInfo          = nullptr;
  // Scene TLAS bound for every ray tracing pass.
  const rtpt::AccelerationStructure*  topLevelAS         = nullptr;
  // Target image written by final shading. Its extent is the render resolution.
  rtpt::RenderTargetView              output {};
  // Selects this frame's descriptor set and parameter buffer, so frames in flight never share them.
  uint32_t                            frameSlot = 0;
  // Time since the previous frame in milliseconds, handed to NRD. Zero lets NRD measure real frame time itself.
  float                               frameTimeMilliseconds = 0.0f;
  // Optional. Receives one timestamp scope per pass that runs this frame; null records no timing.
  rtpt::GpuProfiler*                  profiler = nullptr;
};

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

}  // namespace rtpt
