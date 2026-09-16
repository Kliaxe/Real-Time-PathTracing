#include "PathTracing/Common/ResolveHistory.h"

#include "Denoising/StreamlineRuntime.h"

namespace rtpt
{

namespace
{

// Reported when the renderer was given no Streamline session at all.
const std::string kNoStreamlineReason = "no Streamline session was provided";

}  // namespace

ResolveHistory::ResolveHistory(const CreateInfo& createInfo)
    : m_DenoiserResources(DenoiserResources::CreateInfo { .resources = createInfo.resources, .diagnostics = createInfo.diagnostics })
    , m_NrdDenoiser(NrdDenoiser::CreateInfo { .device = createInfo.device != nullptr ? createInfo.device->Handle() : VK_NULL_HANDLE, .resources = createInfo.resources, .diagnostics = createInfo.diagnostics, .frameSlotCount = createInfo.frameSlotCount })
    , m_RayReconstruction(RayReconstructionDenoiser::CreateInfo { .device = createInfo.device != nullptr ? createInfo.device->Handle() : VK_NULL_HANDLE, .resources = createInfo.resources, .diagnostics = createInfo.diagnostics, .streamline = createInfo.streamline, .frameSlotCount = createInfo.frameSlotCount })
    , m_Streamline(createInfo.streamline)
{
}

void ResolveHistory::Initialize()
{
  m_NrdDenoiser.Initialize();
  m_RayReconstruction.Initialize();
}

void ResolveHistory::Destroy()
{
  // GPU resources
  // The denoisers go before the guide images they read, so no denoiser descriptor outlives an image it points at.

  m_RayReconstruction.Destroy();
  m_NrdDenoiser.Destroy();
  m_DenoiserResources.Destroy();

  // History reset
  // Leaves the history in its freshly constructed state, so a later Initialize starts clean.

  m_Tracker = ResolveHistoryTracker {};
}

bool ResolveHistory::IsReady() const
{
  // Ray Reconstruction is optional hardware support, so only NRD, which runs everywhere, decides readiness.
  return m_NrdDenoiser.IsReady();
}

void ResolveHistory::EnsureViewportResources(VkExtent2D viewportSize)
{
  // Viewport-sized guide images must exist before the renderer's descriptors point at them.
  m_DenoiserResources.EnsureForViewport(viewportSize);
}

ResolveHistory::FrameState ResolveHistory::BeginFrame(const FrameInput& input)
{
  // Frame decisions
  // Every choice that must stay consistent while one frame is recorded is taken once, by the tracker.

  const FrameState frameState = m_Tracker.BeginFrame(ResolveHistoryTracker::FrameInput { .sceneInfo = input.sceneInfo, .topLevelAsAddress = input.topLevelAsAddress, .viewportSize = input.viewportSize, .resolveMode = input.resolveMode, .denoiserSignalsAvailable = input.denoiserSignalsAvailable });

  // NRD frame setup
  // NRD needs its camera history settled before the passes that write its signals are recorded. PrepareFrame records no commands and does not touch the guide images, so it can run before the renderer's descriptor and parameter uploads.
  // Material demodulation is always requested: both renderers demodulate both signals, so the compose pass has to put the material back.
  // Ray Reconstruction needs no preparation; everything it reads is gathered when it evaluates.

  if(frameState.denoiseEnabled && IsNrdResolveMode(frameState.resolveMode))
  {
    m_NrdDenoiser.PrepareFrame(NrdDenoiser::FrameInput { .sceneInfo = input.sceneInfo, .viewportSize = frameState.viewportSize, .historyInvalidated = frameState.denoiserHistoryInvalidated, .enableMaterialDemodulation = true, .settings = input.denoiserSettings, .frameSlot = input.frameSlot, .frameTimeMilliseconds = input.frameTimeMilliseconds, }, m_DenoiserResources);
  }

  return frameState;
}

void ResolveHistory::TransitionGuideImagesForWrite(VkCommandBuffer cmd, VkPipelineStageFlags2 dstStageMask, StorageImageWriteOrdering ordering)
{
  // Only called for denoised frames, so frames without NRD are not charged for guide image setup.

  TransitionStorageImageForWrite(cmd, m_DenoiserResources.GetMotionVectorsImage(), dstStageMask, ordering);
  TransitionStorageImageForWrite(cmd, m_DenoiserResources.GetNormalRoughnessImage(), dstStageMask, ordering);
  TransitionStorageImageForWrite(cmd, m_DenoiserResources.GetBaseColorMetalnessImage(), dstStageMask, ordering);
  TransitionStorageImageForWrite(cmd, m_DenoiserResources.GetViewZImage(), dstStageMask, ordering);
  TransitionStorageImageForWrite(cmd, m_DenoiserResources.GetDiffuseRadianceHitDistanceImage(), dstStageMask, ordering);
  TransitionStorageImageForWrite(cmd, m_DenoiserResources.GetSpecularRadianceHitDistanceImage(), dstStageMask, ordering);
  TransitionStorageImageForWrite(cmd, m_DenoiserResources.GetSpecularDemodulationFactorImage(), dstStageMask, ordering);
}

void ResolveHistory::Denoise(const FrameState& frameState, const DenoiseInput& input)
{
  // The frame snapshot decides, not live settings, so a frame never denoises signals its passes did not write.
  if(!frameState.denoiseEnabled || input.beautyImage == nullptr)
  {
    return;
  }

  // Ray Reconstruction
  // It reads the renderer's noisy radiance and guides directly and writes the finished image into the output target.

  if(IsRayReconstructionResolveMode(frameState.resolveMode))
  {
    const RayReconstructionDenoiser::FrameInput frameInput {
        .cmd                = input.cmd,
        .denoiserInputs     = &m_DenoiserResources,
        .color              = input.beautyImage,
        .output             = input.output,
        .sceneInfo          = input.sceneInfo,
        .sceneInfoAddress   = input.sceneInfoAddress,
        .settings           = input.rayReconstructionSettings,
        .greyLuminance      = input.greyLuminance,
        .historyInvalidated = frameState.denoiserHistoryInvalidated,
        .frameSlot          = input.frameSlot,
    };

    m_RayReconstruction.Denoise(frameInput);
    return;
  }

  // NRD reads the renderer's accumulation image as its noisy beauty input and composes into the output target.
  if(m_NrdDenoiser.IsReady())
  {
    m_NrdDenoiser.Denoise(input.cmd, m_DenoiserResources, input.beautyImage->descriptor.imageView, input.output.view, input.debugView, frameState.viewportSize);
  }
}

void ResolveHistory::FinishFrame(const FrameState& frameState)
{
  // The tracker stores the frame's signatures and counter, and reports when a frame that did not denoise leaves denoiser history stale.
  // Each denoiser's history also goes stale on every frame the other one denoised, so only the denoiser that ran keeps it.
  const ResolveHistoryTracker::FinishResult result = m_Tracker.FinishFrame(frameState);

  if(result.dropDenoiserHistory || !IsNrdResolveMode(frameState.resolveMode))
  {
    m_NrdDenoiser.InvalidateHistory();
  }

  if(result.dropDenoiserHistory || !IsRayReconstructionResolveMode(frameState.resolveMode))
  {
    m_RayReconstruction.InvalidateHistory();
  }
}

void ResolveHistory::InvalidateHistory()
{
  // CPU history restarts through the tracker; the denoisers drop their own temporal history right away.

  m_Tracker.InvalidateHistory();
  m_NrdDenoiser.InvalidateHistory();
  m_RayReconstruction.InvalidateHistory();
}

uint32_t ResolveHistory::GetAccumulatedFrameCount() const
{
  return m_Tracker.GetAccumulatedFrameCount();
}

const DenoiserResources& ResolveHistory::GetDenoiserResources() const
{
  return m_DenoiserResources;
}

bool ResolveHistory::IsRayReconstructionAvailable() const
{
  return m_RayReconstruction.IsReady() && m_RayReconstruction.IsAvailable();
}

const std::string& ResolveHistory::GetRayReconstructionUnavailableReason() const
{
  return m_Streamline != nullptr ? m_Streamline->GetUnavailableReason() : kNoStreamlineReason;
}

}  // namespace rtpt
