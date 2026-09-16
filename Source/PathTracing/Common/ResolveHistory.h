#pragma once

#include <cstdint>
#include <string>

#include <volk.h>

#include "Denoising/DenoiserResources.h"
#include "Denoising/NrdDenoiser.h"
#include "Denoising/RayReconstructionDenoiser.h"
#include "Framework/Vulkan/Barriers.h"
#include "Framework/Vulkan/Diagnostics.h"
#include "Framework/Vulkan/GpuResources.h"
#include "Framework/Vulkan/VulkanDevice.h"
#include "PathTracing/Common/ResolveHistoryTracker.h"
#include "PathTracing/Common/ResolveMode.h"
#include "Rendering/RenderTargetView.h"
#include "Shaders/ShaderIo.h"

namespace rtpt
{

// ResolveHistory
// History handling shared by the ray-traced renderers: decides when accumulated pixels can keep averaging and when a denoiser's temporal history can continue, and drives the denoisers under those decisions.
// PathTracer and ReSTIRPTRenderer each used to carry a copy of this logic, and the copies drifted apart. One owner means the signatures, the counter, and the denoisers always agree.
// The decisions themselves live in ResolveHistoryTracker, which needs no GPU and is covered by CPU tests; this class adds both denoisers and the guide images they share, so a denoiser can only be driven through those decisions.
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
    // Streamline session DLSS Ray Reconstruction runs through. Null leaves Ray Reconstruction unavailable.
    StreamlineRuntime*       streamline     = nullptr;
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

  // DenoiseInput
  // What the renderer hands over once its passes have written this frame's signals. Borrowed for the duration of Denoise only.

  struct DenoiseInput
  {
    // Command buffer the denoiser is recorded into.
    VkCommandBuffer                  cmd                       = VK_NULL_HANDLE;
    // The renderer's noisy radiance for this frame.
    const rtpt::Image*               beautyImage               = nullptr;
    // HDR target the denoised image is written to.
    RenderTargetView                 output {};
    // Which NRD stage is shown. Ray Reconstruction has no intermediate stages and always shows its result.
    DenoiserDebugView                debugView                 = DenoiserDebugView::eFinal;
    // CPU copy of the scene info this frame was traced with.
    const shaderio::GltfSceneInfo*   sceneInfo                 = nullptr;
    // Device address of the same scene info.
    VkDeviceAddress                  sceneInfoAddress          = 0;
    // Ray Reconstruction model and clamp settings.
    const RayReconstructionSettings* rayReconstructionSettings = nullptr;
    // Scene luminance the tonemapper maps to middle grey, for the Ray Reconstruction radiance clamp.
    float                            greyLuminance             = 1.0f;
    // Frame slot being recorded.
    uint32_t                         frameSlot                 = 0;
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
  void       Denoise(const FrameState& frameState, const DenoiseInput& input);
  void       FinishFrame(const FrameState& frameState);
  void       InvalidateHistory();

  // Raw counter, valid between BeginFrame and FinishFrame. Callers that report it gate it on their own resolve mode.
  uint32_t GetAccumulatedFrameCount() const;

  // The guide images the renderer binds in its own descriptor sets.
  const DenoiserResources& GetDenoiserResources() const;

  // Whether DLSS Ray Reconstruction can run on this machine, and why not when it cannot.
  bool               IsRayReconstructionAvailable() const;
  const std::string& GetRayReconstructionUnavailableReason() const;

private:

  // NRD guide and signal images the renderer's shaders write.
  DenoiserResources     m_DenoiserResources;
  // Runs REBLUR on the signals and composes the denoised output.
  NrdDenoiser           m_NrdDenoiser;
  // Runs DLSS Ray Reconstruction on the same signals and writes the denoised output.
  RayReconstructionDenoiser m_RayReconstruction;
  // Borrowed Streamline session, kept for the availability queries. May be null.
  StreamlineRuntime*    m_Streamline = nullptr;
  // Signatures, the accumulated frame counter, and the per-frame history decisions NRD is driven by.
  ResolveHistoryTracker m_Tracker;
};

}  // namespace rtpt
