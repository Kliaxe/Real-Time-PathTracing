#include "ShaderIncludes/ReSTIR/PT/Reservoir.h.slang"

bool TryLoadSpatialNeighbor(uint2 viewportSize,
                            uint2 pixelPosition,
                            uint offsetRotation,
                            uint sampleIndex,
                            ReSTIRPTPrimarySurface centerSurface,
                            out ReSTIRPTPrimarySurface neighborSurface,
                            out ReSTIRPTReservoir neighborReservoir)
{
  neighborSurface   = EmptyPrimarySurface();
  neighborReservoir = EmptyReservoir();

  if(pushConst.neighborOffsetCount == 0u)
  {
    return false;
  }

  const uint   offsetIndex   = (sampleIndex + offsetRotation) % pushConst.neighborOffsetCount;
  const float2 scaledOffset  = neighborOffsetBuffer[offsetIndex].offset * pushConst.spatialResampling.samplingRadius;
  const int2   neighborPixel = int2(pixelPosition) + int2(round(scaledOffset));
  if(!IsInsideViewport(neighborPixel, viewportSize))
  {
    return false;
  }

  const uint neighborPixelIndex = FlattenPixelIndex(uint2(neighborPixel), viewportSize);
  neighborSurface               = currentSurfaceBuffer[neighborPixelIndex];
  neighborReservoir             = LoadSpatialInputReservoir(neighborPixelIndex);
  const float spatialDepthThreshold = max(centerSurface.linearDepth * pushConst.spatialResampling.depthThreshold, 1.0e-4);

  return IsValidPrimarySurface(neighborSurface) && IsValidReservoir(neighborReservoir)
         && neighborSurface.materialIndex == centerSurface.materialIndex
         && abs(neighborSurface.linearDepth - centerSurface.linearDepth) <= spatialDepthThreshold
         && dot(neighborSurface.shadingNormal, centerSurface.shadingNormal) >= pushConst.spatialResampling.normalThreshold
         && neighborReservoir.age <= pushConst.temporalResampling.maxReservoirAge;
}

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
  const ReSTIRPTPrimarySurface centerSurface = currentSurfaceBuffer[pixelIndex];
  ReSTIRPTReservoir centerReservoir          = LoadSpatialInputReservoir(pixelIndex);
  if(!IsValidPrimarySurface(centerSurface))
  {
    currentHistoryReservoirBuffer[pixelIndex] = centerReservoir;
    WriteDebugPixel(pixelIndex, centerReservoir, 1.0, 0.0, debugBuffer[pixelIndex].temporalStatus,
                    uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface));
    return;
  }

  float centerJacobian = 1.0;
  uint  centerStatus   = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
  centerReservoir = ShiftReservoirToSurface(centerReservoir, centerSurface, centerSurface, sceneInfo.cameraPosition, centerJacobian,
                                            centerStatus);
  if(centerReservoir.valid == 0)
  {
    currentHistoryReservoirBuffer[pixelIndex] = centerReservoir;
    WriteDebugPixel(pixelIndex, centerReservoir, centerJacobian, 0.0, debugBuffer[pixelIndex].temporalStatus, centerStatus);
    return;
  }

  uint rng = HashUint((pushConst.rngFrameNumber + 17u) ^ pixelIndex);
  ReSTIRPTReservoir combinedReservoir = EmptyReservoir();
  int selectedNeighborIndex           = -1;
  float selectedJacobian              = centerJacobian;
  uint selectedInverseStatus          = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
  const uint offsetRotation           = pushConst.neighborOffsetCount > 0u ? (rng % pushConst.neighborOffsetCount) : 0u;
  // Spatial reuse always starts from the center pixel's own reservoir, then
  // tries to improve it with a small neighbor set.
  CombineReservoir(combinedReservoir, centerReservoir, centerReservoir.weight * max(centerReservoir.M, 1.0), max(centerReservoir.M, 1.0),
                   RandomFloat(rng), centerReservoir.age > 0u);

  const uint sampleCount = min(pushConst.spatialResampling.numSpatialSamples, pushConst.neighborOffsetCount);
  for(uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
  {
    ReSTIRPTPrimarySurface neighborSurface = EmptyPrimarySurface();
    ReSTIRPTReservoir      neighborReservoir = EmptyReservoir();
    if(!TryLoadSpatialNeighbor(viewportSize, globalIndex, offsetRotation, sampleIndex, centerSurface, neighborSurface, neighborReservoir))
    {
      continue;
    }

    float neighborJacobian = 1.0;
    uint  neighborStatus   = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
    neighborReservoir = ShiftReservoirToSurface(neighborReservoir, neighborSurface, centerSurface, sceneInfo.cameraPosition, neighborJacobian,
                                                neighborStatus);
    if(neighborReservoir.valid == 0)
    {
      continue;
    }

    // Once shifted and re-scored for the center receiver, the neighbor becomes
    // an ordinary RIS candidate.
    if(CombineReservoir(combinedReservoir, neighborReservoir, neighborReservoir.weight * neighborReservoir.M, neighborReservoir.M,
                        RandomFloat(rng), neighborReservoir.age > 0u))
    {
      selectedNeighborIndex = int(sampleIndex);
      selectedJacobian      = neighborJacobian;
    }
  }

  float normalizationNumerator   = combinedReservoir.targetPdf;
  float normalizationDenominator = combinedReservoir.targetPdf * max(centerReservoir.M, 1.0);

  for(uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
  {
    ReSTIRPTPrimarySurface neighborSurface = EmptyPrimarySurface();
    ReSTIRPTReservoir      neighborReservoir = EmptyReservoir();
    if(!TryLoadSpatialNeighbor(viewportSize, globalIndex, offsetRotation, sampleIndex, centerSurface, neighborSurface, neighborReservoir))
    {
      continue;
    }

    float inverseJacobian = 1.0;
    uint  inverseStatus   = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
    ReSTIRPTReservoir selectedAtNeighbor =
        ShiftReservoirToSurface(combinedReservoir, centerSurface, neighborSurface, sceneInfo.cameraPosition, inverseJacobian, inverseStatus);
    const float neighborTargetPdf = IsValidReservoir(selectedAtNeighbor) ? selectedAtNeighbor.targetPdf : 0.0;
    if(selectedNeighborIndex == int(sampleIndex))
    {
      normalizationNumerator = neighborTargetPdf;
      selectedInverseStatus  = inverseStatus;
    }
    normalizationDenominator += neighborTargetPdf * neighborReservoir.M;
  }

  ApplyReservoirBiasCorrectionScale(combinedReservoir, normalizationNumerator, normalizationDenominator);

  const uint finalStatus = (selectedNeighborIndex >= 0 && combinedReservoir.valid == 0u) ? selectedInverseStatus
                                                                                          : uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
  currentHistoryReservoirBuffer[pixelIndex] = combinedReservoir;
  WriteDebugPixel(pixelIndex, combinedReservoir, selectedJacobian, combinedReservoir.M, debugBuffer[pixelIndex].temporalStatus, finalStatus);
}
