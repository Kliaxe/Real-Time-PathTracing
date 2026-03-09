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
  // Spatial reuse always starts from the center pixel's own reservoir, then
  // tries to improve it with a small neighbor set.
  CombineReservoir(combinedReservoir, centerReservoir, centerReservoir.weight * max(centerReservoir.M, 1.0), max(centerReservoir.M, 1.0),
                   RandomFloat(rng), centerReservoir.age > 0u);

  const uint sampleCount = min(pushConst.spatialResampling.numSpatialSamples, pushConst.neighborOffsetCount);
  for(uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
  {
    // The precomputed offset pattern is rotated per pixel so nearby pixels do
    // not always pick the same neighbors in the same order.
    const uint  offsetIndex   = (sampleIndex + (rng % max(pushConst.neighborOffsetCount, 1u))) % max(pushConst.neighborOffsetCount, 1u);
    const float2 scaledOffset = neighborOffsetBuffer[offsetIndex].offset * pushConst.spatialResampling.samplingRadius;
    const int2  neighborPixel = int2(globalIndex) + int2(round(scaledOffset));
    if(!IsInsideViewport(neighborPixel, viewportSize))
    {
      continue;
    }

    const uint neighborPixelIndex = FlattenPixelIndex(uint2(neighborPixel), viewportSize);
    const ReSTIRPTPrimarySurface neighborSurface = currentSurfaceBuffer[neighborPixelIndex];
    ReSTIRPTReservoir            neighborReservoir = LoadSpatialInputReservoir(neighborPixelIndex);
    const float spatialDepthThreshold = max(centerSurface.linearDepth * pushConst.spatialResampling.depthThreshold, 1.0e-4);

    if(!IsValidPrimarySurface(neighborSurface) || !IsValidReservoir(neighborReservoir)
       || neighborSurface.materialIndex != centerSurface.materialIndex
       || abs(neighborSurface.linearDepth - centerSurface.linearDepth) > spatialDepthThreshold
       || dot(neighborSurface.shadingNormal, centerSurface.shadingNormal) < pushConst.spatialResampling.normalThreshold
       || neighborReservoir.age > pushConst.temporalResampling.maxHistoryLength)
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
    CombineReservoir(combinedReservoir, neighborReservoir, neighborReservoir.weight * neighborReservoir.M, neighborReservoir.M,
                     RandomFloat(rng), neighborReservoir.age > 0u);
    centerJacobian = neighborJacobian;
  }

  currentHistoryReservoirBuffer[pixelIndex] = combinedReservoir;
  WriteDebugPixel(pixelIndex, combinedReservoir, centerJacobian, combinedReservoir.M, debugBuffer[pixelIndex].temporalStatus,
                  uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted));
}
