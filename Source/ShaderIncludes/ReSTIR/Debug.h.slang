#ifndef RESTIR_DI_DEBUG_H
#define RESTIR_DI_DEBUG_H

#include "ShaderIncludes/ReSTIR/Common.h.slang"

ReSTIRDebugPixel MakeDefaultDebugPixel()
{
  return (ReSTIRDebugPixel)0;
}

bool IsReSTIRDebugEnabled()
{
  return pushConst.debugView != uint(ReSTIRDebugView::eReSTIRDebugViewDisabled);
}

void WriteReSTIRDebugPixel(uint pixelIndex,
                           float targetPdf,
                           float reservoirWeight,
                           float shiftJacobian,
                           float reuseCount,
                           uint lightKind,
                           uint reservoirAge,
                           uint temporalStatus,
                           uint spatialStatus)
{
  if(!IsReSTIRDebugEnabled())
  {
    return;
  }

  ReSTIRDebugPixel debugPixel = MakeDefaultDebugPixel();
  debugPixel.targetPdf        = targetPdf;
  debugPixel.reservoirWeight  = reservoirWeight;
  debugPixel.shiftJacobian    = shiftJacobian;
  debugPixel.reuseCount       = reuseCount;
  debugPixel.lightKind        = lightKind;
  debugPixel.reservoirAge     = reservoirAge;
  debugPixel.temporalStatus   = temporalStatus;
  debugPixel.spatialStatus    = spatialStatus;
  debugBuffer[pixelIndex]     = debugPixel;
}

float3 GetShiftStatusDebugColor(uint status)
{
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted))
    return float3(0.15, 0.9, 0.25);
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory))
    return float3(0.15, 0.45, 0.95);
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface))
    return float3(0.95, 0.3, 0.2);
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedTargetPdf))
    return float3(0.85, 0.85, 0.2);
  return float3(0.0);
}

float3 VisualizeReSTIRDebugPixel(ReSTIRDebugPixel debugPixel,
                                 uint debugView,
                                 float maxHistoryLength,
                                 float3 fallbackColor,
                                 float3 methodTagColor)
{
  if(debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDisabled))
  {
    return fallbackColor;
  }

  if(debugView == uint(ReSTIRDebugView::eReSTIRDebugViewCandidateKind))
  {
    return methodTagColor;
  }
  if(debugView == uint(ReSTIRDebugView::eReSTIRDebugViewTargetPdf))
  {
    const float value = saturate(log2(debugPixel.targetPdf + 1.0) / 8.0);
    return float3(value, value, value);
  }
  if(debugView == uint(ReSTIRDebugView::eReSTIRDebugViewReservoirWeight))
  {
    const float value = saturate(debugPixel.reservoirWeight * 0.25);
    return float3(value, value, value);
  }
  if(debugView == uint(ReSTIRDebugView::eReSTIRDebugViewReservoirAge))
  {
    const float value = saturate(float(debugPixel.reservoirAge) / max(maxHistoryLength, 1.0));
    return lerp(float3(0.05, 0.1, 0.2), float3(0.95, 0.85, 0.25), value);
  }
  if(debugView == uint(ReSTIRDebugView::eReSTIRDebugViewTemporalStatus))
  {
    return GetShiftStatusDebugColor(debugPixel.temporalStatus);
  }
  if(debugView == uint(ReSTIRDebugView::eReSTIRDebugViewSpatialStatus))
  {
    return GetShiftStatusDebugColor(debugPixel.spatialStatus);
  }
  if(debugView == uint(ReSTIRDebugView::eReSTIRDebugViewShiftJacobian))
  {
    const float value = saturate(abs(log2(max(debugPixel.shiftJacobian, 1.0e-5))) / 6.0);
    return debugPixel.shiftJacobian >= 1.0 ? lerp(float3(0.1, 0.1, 0.1), float3(0.15, 0.95, 0.25), value)
                                           : lerp(float3(0.1, 0.1, 0.1), float3(0.95, 0.25, 0.2), value);
  }
  if(debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDepthDisocclusion))
  {
    return fallbackColor;
  }

  const float reuse = saturate(debugPixel.reuseCount / 8.0);
  return lerp(float3(0.05, 0.05, 0.08), float3(0.15, 0.8, 0.95), reuse);
}

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
  if(!IsReSTIRDebugEnabled())
  {
    return;
  }

  WriteReSTIRDebugPixel(pixelIndex, reservoir.targetPdf, reservoir.weightSum, shiftJacobian, reuseCount, DIGetReservoirDebugTag(reservoir),
                        reservoir.age, temporalStatus, spatialStatus);
}

float3 VisualizeReSTIRDIDebug(uint pixelIndex, float3 fallbackColor)
{
  if(!IsReSTIRDebugEnabled())
  {
    return fallbackColor;
  }

  const ReSTIRDebugPixel debugPixel = debugBuffer[pixelIndex];
  const float maxAge = max(float(diParams.temporalResampling.maxHistoryLength), float(diParams.shading.finalVisibilityMaxAge));
  return VisualizeReSTIRDebugPixel(debugPixel, pushConst.debugView, maxAge, fallbackColor,
                                   GetDILightKindDebugColor(debugPixel.lightKind));
}

#endif
