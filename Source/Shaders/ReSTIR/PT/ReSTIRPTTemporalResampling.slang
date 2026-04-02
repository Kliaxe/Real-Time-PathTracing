#include "ShaderIncludes/ReSTIR/PT/Reservoir.h.slang"
#include "ShaderIncludes/ReSTIR/PT/Common.h.slang"
#define RESTIR_ENABLE_BOILING_FILTER
#define RESTIR_BOILING_FILTER_GROUP_SIZE 8
#include "ReSTIR/Common/BoilingFilter.hlsli"
#include "ReSTIR/Common/Utils/ReservoirAddressing.hlsli"

ReSTIRPTReservoir ApplyPTBoilingFilter(uint2 localIndex, ReSTIRPTReservoir reservoir)
{
  if(pushConst.boilingFilter.enableBoilingFilter == 0u || pushConst.boilingFilter.boilingFilterStrength <= 0.0 || !IsValidReservoir(reservoir))
  {
    return reservoir;
  }

  const float reservoirWeight = reservoir.targetPdf * reservoir.weight;
  if(ReSTIRBoilingFilterInternal(localIndex, pushConst.boilingFilter.boilingFilterStrength, reservoirWeight))
  {
    return EmptyReservoir();
  }

  return reservoir;
}

int2 CalculateTemporalSampleOffset(int sampleIndex, int radius)
{
  sampleIndex &= 7;

  const int mask2 = (sampleIndex >> 1) & 0x01;
  const int mask4 = 1 - ((sampleIndex >> 2) & 0x01);
  const int tmp0  = -1 + 2 * (sampleIndex & 0x01);
  const int tmp1  = 1 - 2 * mask2;
  const int tmp2  = mask4 | mask2;
  const int tmp3  = mask4 | (1 - mask2);

  return int2(tmp0, tmp0 * tmp1) * int2(tmp2, tmp3) * radius;
}

bool IsValidTemporalNeighborSurface(ReSTIRPTPrimarySurface currentSurface,
                                    ReSTIRPTPrimarySurface previousSurface,
                                    float expectedPreviousDepth,
                                    float depthThreshold)
{
  return IsValidPrimarySurface(previousSurface) && previousSurface.materialIndex == currentSurface.materialIndex
         && abs(previousSurface.linearDepth - expectedPreviousDepth) <= depthThreshold
         && dot(previousSurface.shadingNormal, currentSurface.shadingNormal) >= pushConst.temporalResampling.normalThreshold;
}

bool FindTemporalHistoryCandidate(uint2 viewportSize,
                                  uint2 pixelPosition,
                                  uint pixelIndex,
                                  ReSTIRPTPrimarySurface currentSurface,
                                  float3 screenSpaceMotion,
                                  float expectedPreviousDepth,
                                  out ReSTIRPTPrimarySurface previousSurface,
                                  out ReSTIRPTReservoir previousReservoir,
                                  out uint rejectStatus)
{
  previousSurface    = EmptyPrimarySurface();
  previousReservoir  = EmptyReservoir();
  rejectStatus       = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface);

  const float2 pixelCenter      = float2(pixelPosition) + 0.5;
  const int2   previousPixel    = int2(round(pixelCenter + screenSpaceMotion.xy - 0.5));
  const float  depthThreshold   = max(expectedPreviousDepth * pushConst.temporalResampling.depthThreshold, 1.0e-4);
  const int    radius           = 1;
  const int    temporalSampleCount = 5;
  const bool   enableFallbackSampling = pushConst.temporalResampling.enableFallbackSampling != 0;
  const bool   enablePermutationSampling = pushConst.temporalResampling.enablePermutationSampling != 0;
  const int    sampleCount         = temporalSampleCount + (enableFallbackSampling ? 1 : 0);
  const uint   startSeed           = HashUint(pushConst.temporalResampling.uniformRandomNumber ^ pixelIndex);
  const int    temporalSampleStartIdx = int(startSeed & 7u);

  for(int sampleIdx = 0; sampleIdx < sampleCount; ++sampleIdx)
  {
    const bool useFallback = enableFallbackSampling && sampleIdx == temporalSampleCount;

    int2 candidatePixel = previousPixel;
    if(useFallback)
    {
      candidatePixel = int2(pixelPosition);
    }
    else if(sampleIdx > 0)
    {
      candidatePixel += CalculateTemporalSampleOffset(temporalSampleStartIdx + sampleIdx, radius);
    }

    if((enablePermutationSampling && sampleIdx == 0) || useFallback)
    {
      ApplyPermutationSampling(candidatePixel, pushConst.temporalResampling.uniformRandomNumber);
    }

    if(!IsInsideViewport(candidatePixel, viewportSize))
    {
      continue;
    }

    const uint candidatePixelIndex = FlattenPixelIndex(uint2(candidatePixel), viewportSize);
    const ReSTIRPTPrimarySurface candidateSurface = previousSurfaceBuffer[candidatePixelIndex];
    if(!IsValidTemporalNeighborSurface(currentSurface, candidateSurface, expectedPreviousDepth, depthThreshold))
    {
      continue;
    }

    ReSTIRPTReservoir candidateReservoir = previousHistoryReservoirBuffer[candidatePixelIndex];
    if(!IsValidReservoir(candidateReservoir))
    {
      continue;
    }

    if(candidateReservoir.age >= pushConst.temporalResampling.maxReservoirAge)
    {
      rejectStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedHistoryAge);
      continue;
    }

    previousSurface    = candidateSurface;
    previousReservoir  = candidateReservoir;
    rejectStatus       = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
    return true;
  }

  return false;
}

[numthreads(8, 8, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID, uint2 localIndex : SV_GroupThreadID)
{
  const GltfSceneInfo sceneInfo    = pushConst.sceneInfoAddress[0];
  const uint2         viewportSize = uint2(sceneInfo.viewportSize);
  if(any(globalIndex >= viewportSize))
  {
    return;
  }

  const uint pixelIndex = FlattenPixelIndex(globalIndex, viewportSize);
  // Temporal always starts from the current frame's locally generated
  // candidate, then optionally combines one reprojected history candidate.
  ReSTIRPTReservoir currentReservoir = initialReservoirBuffer[pixelIndex];
  if(!HasTemporalHistory())
  {
    currentReservoir = ApplyPTBoilingFilter(localIndex, currentReservoir);
    StoreTemporalOutput(pixelIndex, currentReservoir);
    WriteDebugPixel(pixelIndex, currentReservoir, 1.0, currentReservoir.valid != 0u ? 1.0 : 0.0,
                    uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory), debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  const ReSTIRPTPrimarySurface currentSurface = currentSurfaceBuffer[pixelIndex];
  if(!IsValidPrimarySurface(currentSurface))
  {
    currentReservoir = ApplyPTBoilingFilter(localIndex, currentReservoir);
    StoreTemporalOutput(pixelIndex, currentReservoir);
    WriteDebugPixel(pixelIndex, currentReservoir, 1.0, 0.0, uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface),
                    debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  float currentJacobian = 1.0;
  uint  currentStatus   = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
  // Reevaluate the current candidate against the current receiver first so the
  // pass handles invalid local candidates and shifted history uniformly.
  currentReservoir = ShiftReservoirToSurface(currentReservoir, currentSurface, currentSurface, sceneInfo.cameraPosition, currentJacobian,
                                             currentStatus);
  const bool hasCurrentCandidate = currentReservoir.valid != 0u;

  const float2 pixelCenter = float2(globalIndex) + 0.5;
  float3 temporalMotionVector = float3(0.0, 0.0, 0.0);
  if(!TryComputeReSTIRTemporalMotionVector(pixelCenter, currentSurface.worldPosition, currentSurface.linearDepth, sceneInfo,
                                           temporalMotionVector))
  {
    currentReservoir = ApplyPTBoilingFilter(localIndex, currentReservoir);
    StoreTemporalOutput(pixelIndex, currentReservoir);
    WriteDebugPixel(pixelIndex, currentReservoir, currentJacobian, currentReservoir.valid != 0u ? 1.0 : 0.0,
                    uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory), debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  const float3 screenSpaceMotion = ConvertMotionVectorToPixelSpace(pixelCenter, temporalMotionVector, sceneInfo);
  const float expectedPreviousDepth = currentSurface.linearDepth + screenSpaceMotion.z;
  ReSTIRPTPrimarySurface previousSurface = EmptyPrimarySurface();
  ReSTIRPTReservoir previousReservoir = EmptyReservoir();
  uint rejectStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface);
  if(!FindTemporalHistoryCandidate(viewportSize, globalIndex, pixelIndex, currentSurface, screenSpaceMotion, expectedPreviousDepth,
                                   previousSurface, previousReservoir, rejectStatus))
  {
    currentReservoir = ApplyPTBoilingFilter(localIndex, currentReservoir);
    StoreTemporalOutput(pixelIndex, currentReservoir);
    WriteDebugPixel(pixelIndex, currentReservoir, currentJacobian, currentReservoir.valid != 0u ? 1.0 : 0.0, rejectStatus,
                    debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  float previousJacobian = 1.0;
  uint  previousStatus   = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
  previousReservoir = ShiftReservoirToSurface(previousReservoir, previousSurface, currentSurface, sceneInfo.cameraPosition, previousJacobian,
                                              previousStatus);
  if(previousReservoir.valid == 0)
  {
    currentReservoir = ApplyPTBoilingFilter(localIndex, currentReservoir);
    StoreTemporalOutput(pixelIndex, currentReservoir);
    WriteDebugPixel(pixelIndex, currentReservoir, previousJacobian, currentReservoir.valid != 0u ? 1.0 : 0.0, previousStatus,
                    debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  uint rng = HashUint(pushConst.rngFrameNumber ^ pixelIndex);
  ReSTIRPTReservoir combinedReservoir = EmptyReservoir();
  bool selectedPreviousSample = false;
  // RIS first inserts the current-frame candidate, then one history candidate
  // whose effective sample count is capped by the max history length.
  if(hasCurrentCandidate)
  {
    CombineReservoir(combinedReservoir, currentReservoir, currentReservoir.weight * currentReservoir.M, currentReservoir.M, RandomFloat(rng),
                     false);
  }

  const float historyM = min(previousReservoir.M, float(pushConst.temporalResampling.maxHistoryLength));
  previousReservoir.M = historyM;
  previousReservoir.age += 1u;
  selectedPreviousSample =
      CombineReservoir(combinedReservoir, previousReservoir, previousReservoir.weight * historyM, historyM, RandomFloat(rng), true);

  float normalizationNumerator   = combinedReservoir.targetPdf;
  float normalizationDenominator = hasCurrentCandidate ? (combinedReservoir.targetPdf * currentReservoir.M) : 0.0;

  float inverseJacobian = 1.0;
  uint  inverseStatus   = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
  ReSTIRPTReservoir selectedAtPrevious =
      ShiftReservoirToSurface(combinedReservoir, currentSurface, previousSurface, sceneInfo.prevCameraPosition, inverseJacobian, inverseStatus);
  const float temporalTargetPdf = IsValidReservoir(selectedAtPrevious) ? selectedAtPrevious.targetPdf : 0.0;
  if(selectedPreviousSample)
  {
    normalizationNumerator = temporalTargetPdf;
  }
  normalizationDenominator += temporalTargetPdf * historyM;
  ApplyReservoirBiasCorrectionScale(combinedReservoir, normalizationNumerator, normalizationDenominator);

  uint finalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
  if(selectedPreviousSample && combinedReservoir.valid == 0u)
  {
    finalStatus = inverseStatus;
  }
  else if(!hasCurrentCandidate && !selectedPreviousSample)
  {
    finalStatus = currentStatus;
  }

  combinedReservoir = ApplyPTBoilingFilter(localIndex, combinedReservoir);
  StoreTemporalOutput(pixelIndex, combinedReservoir);
  const float debugJacobian = selectedPreviousSample ? previousJacobian : currentJacobian;
  WriteDebugPixel(pixelIndex, combinedReservoir, debugJacobian, combinedReservoir.M, finalStatus, debugBuffer[pixelIndex].spatialStatus);
}
