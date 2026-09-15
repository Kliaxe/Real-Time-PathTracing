#include "PathTracing/Common/ResolveHistoryTracker.h"

#include <cstring>

namespace rtpt
{

ResolveHistoryTracker::FrameState ResolveHistoryTracker::BeginFrame(const FrameInput& input)
{
  // Frame decisions
  // Every choice that must stay consistent while one frame is recorded is taken once here.

  FrameState frameState {};

  frameState.viewportSize          = input.viewportSize;
  frameState.accumulationSignature = MakeAccumulationSignature(input);
  frameState.denoiserSignature     = MakeDenoiserSignature(input);
  frameState.accumulateEnabled     = IsAccumulationResolveMode(input.resolveMode);
  frameState.denoiseEnabled        = IsDenoiseResolveMode(input.resolveMode) && input.denoiserSignalsAvailable;

  // Accumulation history
  // Accumulation history is only valid while the camera, scene, and environment match.
  // The restart is gated on accumulate mode: outside it FinishFrame already holds the counter at zero, and a renderer's own reuse history must survive camera moves.

  const bool accumulationSignatureChanged = !m_HasAccumulationSignature || !SignaturesMatch(frameState.accumulationSignature, m_LastAccumulationSignature);

  frameState.accumulationRestarted = m_HistoryInvalidated || (frameState.accumulateEnabled && accumulationSignatureChanged);

  if(frameState.accumulationRestarted)
  {
    m_AccumulatedFrames = 0;
  }

  // Denoiser history
  // NRD history tracks a slightly smaller set of state than final radiance accumulation.

  const bool denoiserSignatureChanged = !m_HasDenoiserSignature || !SignaturesMatch(frameState.denoiserSignature, m_LastDenoiserSignature);

  frameState.denoiserHistoryInvalidated = m_HistoryInvalidated || (frameState.denoiseEnabled && denoiserSignatureChanged);

  return frameState;
}

ResolveHistoryTracker::FinishResult ResolveHistoryTracker::FinishFrame(const FrameState& frameState)
{
  // Accumulation history
  // Store history signatures after all passes used the current frame state.

  m_LastAccumulationSignature = frameState.accumulationSignature;
  m_HasAccumulationSignature  = true;
  m_HistoryInvalidated        = false;
  m_AccumulatedFrames         = frameState.accumulateEnabled ? (m_AccumulatedFrames + 1) : 0;

  // Denoiser history
  // If NRD is skipped for one or more frames, its temporal history no longer represents the image sequence. Restart cleanly next time denoising is enabled.
  // This also covers a renderer whose signals are unavailable, such as ReSTIR PT's reference view: that image comes from a different estimator, and NRD would otherwise reproject frames from both.
  // Dropping NRD's history here rather than right after the passes is equivalent, because NRD only reads it in the next PrepareFrame.

  FinishResult result {};

  if(frameState.denoiseEnabled)
  {
    m_LastDenoiserSignature = frameState.denoiserSignature;
    m_HasDenoiserSignature  = true;
  }
  else
  {
    m_HasDenoiserSignature     = false;
    result.dropDenoiserHistory = true;
  }

  return result;
}

void ResolveHistoryTracker::InvalidateHistory()
{
  // Shader-visible accumulation restarts through the counter immediately; the flag makes the next BeginFrame restart both histories.

  m_AccumulatedFrames        = 0;
  m_HistoryInvalidated       = true;
  m_HasAccumulationSignature = false;
  m_HasDenoiserSignature     = false;
}

uint32_t ResolveHistoryTracker::GetAccumulatedFrameCount() const
{
  return m_AccumulatedFrames;
}

ResolveHistoryTracker::AccumulationSignature ResolveHistoryTracker::MakeAccumulationSignature(const FrameInput& input)
{
  // Kept explicit so adding or removing a history dependency is easy to review.
  AccumulationSignature signature {};

  signature.viewProjMatrix          = input.sceneInfo->viewProjMatrix;
  signature.viewProjInvMatrix       = input.sceneInfo->viewProjInvMatrix;
  signature.viewInvMatrix           = input.sceneInfo->viewInvMatrix;
  signature.cameraPosition          = input.sceneInfo->cameraPosition;
  signature.useSky                  = input.sceneInfo->useSky;
  signature.useHdrEnv               = input.sceneInfo->useHdrEnv;
  signature.environmentTextureIndex = input.sceneInfo->environmentTextureIndex;
  signature.backgroundColor         = input.sceneInfo->backgroundColor;
  signature.skySimpleParam          = input.sceneInfo->skySimpleParam;
  signature.topLevelAsAddress       = input.topLevelAsAddress;
  signature.viewportSize            = input.viewportSize;

  return signature;
}

ResolveHistoryTracker::DenoiserSignature ResolveHistoryTracker::MakeDenoiserSignature(const FrameInput& input)
{
  // Intentionally smaller than the accumulation signature.
  DenoiserSignature signature {};

  signature.useSky                  = input.sceneInfo->useSky;
  signature.useHdrEnv               = input.sceneInfo->useHdrEnv;
  signature.environmentTextureIndex = input.sceneInfo->environmentTextureIndex;
  signature.backgroundColor         = input.sceneInfo->backgroundColor;
  signature.skySimpleParam          = input.sceneInfo->skySimpleParam;
  signature.topLevelAsAddress       = input.topLevelAsAddress;
  signature.viewportSize            = input.viewportSize;

  return signature;
}

bool ResolveHistoryTracker::SignaturesMatch(const AccumulationSignature& left, const AccumulationSignature& right)
{
  // Bytewise on purpose: the explicit padding makes every byte meaningful, and a memberwise float compare would treat -0 and +0 as equal and NaN as never equal.
  return std::memcmp(&left, &right, sizeof(AccumulationSignature)) == 0;
}

bool ResolveHistoryTracker::SignaturesMatch(const DenoiserSignature& left, const DenoiserSignature& right)
{
  // Bytewise for the same reason as the accumulation overload.
  return std::memcmp(&left, &right, sizeof(DenoiserSignature)) == 0;
}

}  // namespace rtpt
