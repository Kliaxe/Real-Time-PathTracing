#ifndef RESTIR_GI_DEBUG_H
#define RESTIR_GI_DEBUG_H

#include "ShaderIncludes/ReSTIR/Common/Debug.h.slang"

uint GIGetReservoirDebugTag(ReSTIRGIReservoir reservoir)
{
  return reservoir.sampleKind;
}

float3 GetGISampleKindDebugColor(uint sampleKind)
{
  if(sampleKind == uint(ReSTIRGISampleKind::eReSTIRGISampleKindEnvironment))
    return float3(0.25, 0.65, 0.95);
  return float3(0.95, 0.55, 0.2);
}

void WriteGIDebugPixel(uint pixelIndex,
                       GISurface surface,
                       ReSTIRGIReservoir reservoir,
                       float shiftJacobian,
                       float reuseCount,
                       uint temporalStatus,
                       uint spatialStatus)
{
  const float targetPdf = (GIIsSurfaceValid(surface) && IsValidGIReservoir(reservoir)) ? GIGetSampleTargetPdfForSurface(reservoir, surface) : 0.0;
  WriteReSTIRDebugPixel(pixelIndex, targetPdf, reservoir.weightSum, shiftJacobian, reuseCount, GIGetReservoirDebugTag(reservoir), reservoir.age,
                        temporalStatus, spatialStatus);
}

float3 VisualizeReSTIRGIDebug(uint pixelIndex, float3 fallbackColor)
{
  const ReSTIRDebugPixel debugPixel = debugBuffer[pixelIndex];
  return VisualizeReSTIRCommonDebugPixel(debugPixel, pushConst.debugView, float(giParams.temporalResampling.maxReservoirAge), fallbackColor,
                                         GetGISampleKindDebugColor(debugPixel.candidateKind));
}

#endif
