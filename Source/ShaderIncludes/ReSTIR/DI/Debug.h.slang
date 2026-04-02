#ifndef RESTIR_DI_DEBUG_H
#define RESTIR_DI_DEBUG_H

#include "ShaderIncludes/ReSTIR/Common/Debug.h.slang"

uint DIGetReservoirDebugTag(ReSTIRDIReservoir reservoir)
{
  if(!IsValidDIReservoir(reservoir))
  {
    return kReSTIRDILightKindInvalid;
  }

  return DILoadLightInfo(GetDIReservoirLightIndex(reservoir), false).kind;
}

float3 GetDILightKindDebugColor(uint lightKind)
{
  if(lightKind == kReSTIRDILightKindEnvironment)
    return float3(0.25, 0.65, 0.95);
  if(lightKind == kReSTIRDILightKindEmissiveTriangle)
    return float3(0.95, 0.55, 0.2);
  return float3(0.0);
}

void WriteDIDebugPixel(uint pixelIndex,
                       ReSTIRDIReservoir reservoir,
                       float shiftJacobian,
                       float reuseCount,
                       uint temporalStatus,
                       uint spatialStatus)
{
  WriteReSTIRDebugPixel(pixelIndex, reservoir.targetPdf, reservoir.weightSum, shiftJacobian, reuseCount, DIGetReservoirDebugTag(reservoir),
                        reservoir.age, temporalStatus, spatialStatus);
}

float3 VisualizeReSTIRDIDebug(uint pixelIndex, float3 fallbackColor)
{
  const ReSTIRDebugPixel debugPixel = debugBuffer[pixelIndex];
  const float maxAge = max(float(diParams.temporalResampling.maxHistoryLength), float(diParams.shading.finalVisibilityMaxAge));
  return VisualizeReSTIRCommonDebugPixel(debugPixel, pushConst.debugView, maxAge, fallbackColor,
                                         GetDILightKindDebugColor(debugPixel.candidateKind));
}

#endif
