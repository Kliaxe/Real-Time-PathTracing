#include "ShaderIncludes/ReSTIR/PT/Reservoir.h.slang"

[numthreads(8, 8, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
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
    StoreTemporalOutput(pixelIndex, currentReservoir);
    WriteDebugPixel(pixelIndex, currentReservoir, 1.0, currentReservoir.valid != 0u ? 1.0 : 0.0,
                    uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory), debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  const ReSTIRPTPrimarySurface currentSurface = currentSurfaceBuffer[pixelIndex];
  if(!IsValidPrimarySurface(currentSurface))
  {
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
  if(currentReservoir.valid == 0)
  {
    StoreTemporalOutput(pixelIndex, currentReservoir);
    WriteDebugPixel(pixelIndex, currentReservoir, currentJacobian, 0.0, currentStatus, debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  const float3 reprojectedPixel = ReprojectWorldPositionToPixel(currentSurface.worldPosition, sceneInfo);
  const int2   previousPixel    = int2(round(reprojectedPixel.xy));
  if(!IsInsideViewport(previousPixel, viewportSize))
  {
    StoreTemporalOutput(pixelIndex, currentReservoir);
    WriteDebugPixel(pixelIndex, currentReservoir, currentJacobian, currentReservoir.valid != 0u ? 1.0 : 0.0,
                    uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface), debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  const uint previousPixelIndex = FlattenPixelIndex(uint2(previousPixel), viewportSize);
  const ReSTIRPTPrimarySurface previousSurface   = previousSurfaceBuffer[previousPixelIndex];
  ReSTIRPTReservoir            previousReservoir = previousHistoryReservoirBuffer[previousPixelIndex];
  // Depth is compared in camera-space distance form because the surface buffer
  // stores a scalar linear depth for reuse validation.
  const float expectedPreviousDepth = length(currentSurface.worldPosition - sceneInfo.prevCameraPosition);
  const float depthThreshold        = max(expectedPreviousDepth * pushConst.temporalResampling.depthThreshold, 1.0e-4);

  if(!IsValidPrimarySurface(previousSurface) || !IsValidReservoir(previousReservoir)
     || previousSurface.materialIndex != currentSurface.materialIndex
     || abs(previousSurface.linearDepth - expectedPreviousDepth) > depthThreshold
     || dot(previousSurface.shadingNormal, currentSurface.shadingNormal) < pushConst.temporalResampling.normalThreshold
     || previousReservoir.age >= pushConst.temporalResampling.maxReservoirAge)
  {
    const uint rejectStatus = previousReservoir.age >= pushConst.temporalResampling.maxReservoirAge
                                  ? uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedHistoryAge)
                                  : uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface);
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
    StoreTemporalOutput(pixelIndex, currentReservoir);
    WriteDebugPixel(pixelIndex, currentReservoir, previousJacobian, currentReservoir.valid != 0u ? 1.0 : 0.0, previousStatus,
                    debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  uint rng = HashUint(pushConst.rngFrameNumber ^ pixelIndex);
  ReSTIRPTReservoir combinedReservoir = EmptyReservoir();
  // RIS first inserts the current-frame candidate, then one history candidate
  // whose effective sample count is capped by the max history length.
  CombineReservoir(combinedReservoir, currentReservoir, currentReservoir.weight * max(currentReservoir.M, 1.0), max(currentReservoir.M, 1.0),
                   RandomFloat(rng), false);

  const float historyM = min(previousReservoir.M, float(pushConst.temporalResampling.maxHistoryLength));
  CombineReservoir(combinedReservoir, previousReservoir, previousReservoir.weight * historyM, historyM, RandomFloat(rng), true);

  StoreTemporalOutput(pixelIndex, combinedReservoir);
  WriteDebugPixel(pixelIndex, combinedReservoir, previousJacobian, combinedReservoir.M,
                  uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted), debugBuffer[pixelIndex].spatialStatus);
}
