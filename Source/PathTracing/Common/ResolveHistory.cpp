#include "PathTracing/Common/ResolveHistory.h"

namespace rtpt
{

ResolveHistory::ResolveHistory(const CreateInfo& createInfo)
    : m_DenoiserResources(DenoiserResources::CreateInfo { .resources = createInfo.resources, .diagnostics = createInfo.diagnostics })
    , m_NrdDenoiser(NrdDenoiser::CreateInfo { .device = createInfo.device != nullptr ? createInfo.device->Handle() : VK_NULL_HANDLE, .resources = createInfo.resources, .diagnostics = createInfo.diagnostics, .frameSlotCount = createInfo.frameSlotCount })
{
}

void ResolveHistory::Initialize()
{
  m_NrdDenoiser.Initialize();
}

void ResolveHistory::Destroy()
{
  // GPU resources
  // NRD goes before the guide images it reads, so no NRD descriptor outlives an image it points at.

  m_NrdDenoiser.Destroy();
  m_DenoiserResources.Destroy();

  // History reset
  // Leaves the history in its freshly constructed state, so a later Initialize starts clean.

  m_Tracker = ResolveHistoryTracker {};
}

bool ResolveHistory::IsReady() const
{
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

  if(frameState.denoiseEnabled)
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

void ResolveHistory::Denoise(VkCommandBuffer cmd, const FrameState& frameState, VkImageView beautyImageView, VkImageView outputImageView, DenoiserDebugView debugView)
{
  // The frame snapshot decides, not live settings, so a frame never denoises signals its passes did not write.
  if(!frameState.denoiseEnabled || !m_NrdDenoiser.IsReady())
  {
    return;
  }

  // NRD reads the renderer's accumulation image as its noisy beauty input and composes into the output target.
  m_NrdDenoiser.Denoise(cmd, m_DenoiserResources, beautyImageView, outputImageView, debugView, frameState.viewportSize);
}

void ResolveHistory::FinishFrame(const FrameState& frameState)
{
  // The tracker stores the frame's signatures and counter, and reports when a frame that did not denoise leaves NRD's history stale.
  const ResolveHistoryTracker::FinishResult result = m_Tracker.FinishFrame(frameState);

  if(result.dropDenoiserHistory)
  {
    m_NrdDenoiser.InvalidateHistory();
  }
}

void ResolveHistory::InvalidateHistory()
{
  // CPU history restarts through the tracker; NRD drops its own temporal history right away.

  m_Tracker.InvalidateHistory();
  m_NrdDenoiser.InvalidateHistory();
}

uint32_t ResolveHistory::GetAccumulatedFrameCount() const
{
  return m_Tracker.GetAccumulatedFrameCount();
}

const DenoiserResources& ResolveHistory::GetDenoiserResources() const
{
  return m_DenoiserResources;
}

}  // namespace rtpt
