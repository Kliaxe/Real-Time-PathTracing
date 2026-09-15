#pragma once

#include <cstdint>

#include <volk.h>

#include "Denoising/DenoiserResources.h"
#include "Denoising/NrdDenoiser.h"
#include "Framework/Vulkan/Barriers.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "PathTracing/Common/ResolveHistoryTracker.h"
#include "PathTracing/Common/ResolveMode.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

// ResolveHistory
// History handling shared by the ray-traced renderers: decides when accumulated pixels can keep averaging and when NRD's temporal history can continue, and drives NRD under those decisions.
// PathTracer and ReSTIRPTRenderer each used to carry a copy of this logic, and the copies drifted apart. One owner means the signatures, the counter, and NRD always agree.
// The decisions themselves live in ResolveHistoryTracker, which needs no GPU and is covered by CPU tests; this class adds the NRD denoiser and its guide images, so NRD can only be driven through those decisions.
// Renderers keep what is specific to them - pipelines, their accumulation image, RNG sequence, and ReSTIR's reservoirs - and restart their own history from FrameState::accumulationRestarted.

class ResolveHistory
{
public:

  // CreateInfo
  // Lifetime dependencies borrowed from the owning renderer, which borrows them from Application.

  struct CreateInfo
  {
    // Logical device NRD's pipelines and pools are created on.
    rtpt::VulkanDevice*      device         = nullptr;
    // Allocates the guide images and NRD's own images. DenoiserResources throws if this is null.
    rtpt::ResourceAllocator* resources      = nullptr;
    // Optional. Names Vulkan objects for debugging tools when present.
    const rtpt::Diagnostics* diagnostics    = nullptr;
    // Frames that can be in flight at once. NRD keeps one descriptor pool and constant buffer per slot.
    uint32_t                 frameSlotCount = 0;
  };

  // FrameInput
  // One-frame state the history decisions are taken from. Borrowed for the duration of BeginFrame only.

  struct FrameInput
  {
    // CPU copy of the scene info: camera and environment for the signatures, camera matrices for NRD.
    const shaderio::GltfSceneInfo* sceneInfo                = nullptr;
    // TLAS address, copied into both signatures. It is not what catches a scene rebuild; see ResolveHistoryTracker::AccumulationSignature::topLevelAsAddress.
    VkDeviceAddress                topLevelAsAddress        = 0;
    // Resolution this frame is rendered at.
    VkExtent2D                     viewportSize {};
    // Decides whether this frame accumulates, denoises, or neither.
    RenderResolveMode              resolveMode              = RenderResolveMode::eOff;
    // False when the renderer cannot produce NRD signals that describe its beauty image this frame, such as ReSTIR PT's reference path tracer view.
    // NRD then stands down for the frame and its history is dropped, exactly as if denoising were off.
    bool                           denoiserSignalsAvailable = true;
    // REBLUR settings for this frame. Null keeps the settings NRD applied last.
    const DenoiserSettings*        denoiserSettings         = nullptr;
    // Frame slot being recorded; selects NRD's per-slot resources.
    uint32_t                       frameSlot                = 0;
    // Time since the previous frame in milliseconds, forwarded to NRD. Zero lets NRD measure it with its own timer.
    float                          frameTimeMilliseconds    = 0.0f;
  };

  // The tracker's snapshot is the frame state renderers pass back into Denoise and FinishFrame.
  using FrameState = ResolveHistoryTracker::FrameState;

  explicit ResolveHistory(const CreateInfo& createInfo);

  void Initialize();
  void Destroy();
  bool IsReady() const;

  void       EnsureViewportResources(VkExtent2D viewportSize);
  FrameState BeginFrame(const FrameInput& input);
  void       TransitionGuideImagesForWrite(VkCommandBuffer cmd, VkPipelineStageFlags2 dstStageMask, StorageImageWriteOrdering ordering);
  void       Denoise(VkCommandBuffer cmd, const FrameState& frameState, VkImageView beautyImageView, VkImageView outputImageView, DenoiserDebugView debugView);
  void       FinishFrame(const FrameState& frameState);
  void       InvalidateHistory();

  // Raw counter, valid between BeginFrame and FinishFrame. Callers that report it gate it on their own resolve mode.
  uint32_t GetAccumulatedFrameCount() const;

  // The guide images the renderer binds in its own descriptor sets.
  const DenoiserResources& GetDenoiserResources() const;

private:

  // NRD guide and signal images the renderer's shaders write.
  DenoiserResources     m_DenoiserResources;
  // Runs REBLUR on the signals and composes the denoised output.
  NrdDenoiser           m_NrdDenoiser;
  // Signatures, the accumulated frame counter, and the per-frame history decisions NRD is driven by.
  ResolveHistoryTracker m_Tracker;
};

}  // namespace rtpt
