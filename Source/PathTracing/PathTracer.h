#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "Scene/SceneGpuResources.h"
#include "PathTracing/Common/ResolveHistory.h"
#include "PathTracing/Common/ResolveMode.h"
#include "Framework/Vulkan/Descriptors.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/ShaderBindingTable.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "Rendering/RenderTargetView.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

class SpatiotemporalBlueNoise;

// PathTracer
// Ground-truth and direct-sampling baseline renderer. It owns the ray tracing pipeline, accumulation target, and NRD resources used by the baseline modes.
// CPU responsibility: prepare Vulkan state and record one ray tracing dispatch.
// Shader responsibility: trace paths, accumulate radiance, and write NRD signals.

class PathTracer
{
public:

  // CreateInfo
  // Lifetime dependencies owned by Application. PathTracer borrows them and must be destroyed first.

  struct CreateInfo
  {
    // Logical device and its ray tracing support queries.
    rtpt::VulkanDevice*      device                = nullptr;
    // Allocates the accumulation image, SBT, and NRD resources.
    rtpt::ResourceAllocator* resources             = nullptr;
    // Optional. Names Vulkan objects for debugging tools when present.
    const rtpt::Diagnostics* diagnostics           = nullptr;
    // Blue noise texture that seeds the ray generation shader's per-pixel random numbers.
    const rtpt::SpatiotemporalBlueNoise* blueNoise = nullptr;
    // Frames that can be in flight at once. One descriptor set is allocated per slot.
    uint32_t                 frameSlotCount        = 0;
    // Size of the bindless texture arrays in the descriptor layout.
    uint32_t                 maxTextureDescriptors = 0;
  };

  // RenderInput
  // One-frame borrowed state. PathTracer records commands but owns none of these objects.

  struct RenderInput
  {
    // Command buffer the dispatch is recorded into.
    VkCommandBuffer                     cmd                = VK_NULL_HANDLE;
    // GPU scene buffers; the scene info address reaches the shader through push constants.
    const rtpt::GltfSceneResource*      sceneResource      = nullptr;
    // CPU copy of the scene info, used for history signatures and NRD camera state.
    const shaderio::GltfSceneInfo*      sceneInfo          = nullptr;
    // Scene TLAS bound to the ray tracing shaders.
    const rtpt::AccelerationStructure*  topLevelAS         = nullptr;
    // Target image written by ray generation. Its extent is the render resolution.
    rtpt::RenderTargetView              output {};
    // Selects this frame's descriptor set, clamped to the allocated slot count.
    uint32_t                            frameSlot = 0;
    // Time since the previous frame in milliseconds, handed to NRD. Zero lets NRD measure real frame time itself.
    float                               frameTimeMilliseconds = 0.0f;
  };

  // Settings
  // User-facing controls read at the start of every frame.

  struct Settings
  {
    // Resolve mode decides whether the noisy image is raw, accumulated, or denoised.
    RenderResolveMode resolveMode       = RenderResolveMode::eOff;
    // Which NRD input or output is shown when resolving with the denoiser.
    DenoiserDebugView denoiserDebugView = DenoiserDebugView::eFinal;
    // NRD REBLUR settings, including the hit distance normalization the shader also uses.
    DenoiserSettings  denoiserSettings { .hitDistanceReconstructionMode = HitDistanceReconstructionMode::eArea5x5 };
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

  rtpt::DescriptorPack&       GetDescriptorPack();
  const rtpt::DescriptorPack& GetDescriptorPack() const;

  void Render(const RenderInput& input);

private:

  // Creation
  // Called once from Initialize, in declaration order.

  void QueryRayTracingProperties();
  void CreateDescriptorSetLayout();
  void CreatePipelineLayout();
  void CreateRayTracingPipeline();
  void CreateShaderBindingTable();

  // Frame recording
  // Helpers for Render; see PathTracer::Render for the order they run in.

  bool CanRender(const RenderInput& input) const;
  void EnsureViewportResources(VkExtent2D viewportSize);
  void PrepareStorageImages(const RenderInput& input, const ResolveHistory::FrameState& frameState);
  shaderio::PathTracePushConstant BuildPushConstant(const RenderInput& input, const ResolveHistory::FrameState& frameState);
  void RecordPathTracePass(const RenderInput& input, const shaderio::PathTracePushConstant& pushConstant);
  void UpdateFrameDescriptors(const RenderInput& input);
  void CreateOrResizeAccumulationImage(VkExtent2D size);
  void DestroyAccumulationImage();

  // Lifetime dependencies, borrowed through CreateInfo.

  // Logical device the pipeline and descriptor sets are created on.
  rtpt::VulkanDevice*      m_Device = nullptr;
  // Allocator for the accumulation image and SBT.
  rtpt::ResourceAllocator* m_Resources = nullptr;
  // Optional debug naming; null disables it.
  const rtpt::Diagnostics* m_Diagnostics = nullptr;
  // Blue noise texture bound for the ray generation shader's RNG seeds.
  const rtpt::SpatiotemporalBlueNoise* m_BlueNoise = nullptr;
  // Frames that can be in flight at once; one descriptor set each.
  uint32_t                 m_FrameSlotCount = 0;
  // Size of the bindless texture arrays in the descriptor layout.
  uint32_t                 m_MaxTextureDescriptors = 0;

  // History state

  // Advances every rendered frame to decorrelate random samples, even when accumulation is off.
  uint32_t                 m_RngFrameNumber        = 0;
  // Device limit comes from Vulkan; pipeline limit is the depth this renderer requested.
  uint32_t                 m_DeviceBounceLimit     = 0;
  // Bounces the ray tracing pipeline was created for. Never above m_DeviceBounceLimit.
  uint32_t                 m_PipelineBounceLimit   = 0;
  // User-facing settings, read at the start of every frame.
  Settings                 m_Settings {};

  // Vulkan state

  // One descriptor set per frame slot, also updated by Application with scene textures.
  rtpt::DescriptorPack        m_DescPack;
  // Descriptor set and push constant ABI shared by every ray tracing stage.
  VkPipelineLayout            m_PipelineLayout = VK_NULL_HANDLE;
  // The single ray tracing pipeline that traces, accumulates, and writes NRD signals.
  VkPipeline                  m_Pipeline       = VK_NULL_HANDLE;
  // Vulkan ray tracing needs an SBT that maps TraceRay indices to shader groups.
  rtpt::ShaderBindingTable    m_Sbt;
  // RGBA32F running average of path radiance, and the noisy beauty input NRD reads.
  rtpt::Image                 m_AccumulationImage;
  // Accumulation counter, history signatures, and the NRD denoiser with the guide and signal images the ray generation shader writes.
  ResolveHistory              m_History;
  // Device ray tracing limits. Recursion depth bounds the bounce limits, and the SBT is built against these properties.
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtProperties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
};

}  // namespace rtpt
