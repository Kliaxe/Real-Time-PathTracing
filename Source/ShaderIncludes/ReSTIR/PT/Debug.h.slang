#ifndef RESTIR_PT_DEBUG_H
#define RESTIR_PT_DEBUG_H

#include "ShaderIncludes/ReSTIR/PT/Shift.h.slang"

ReSTIRDebugPixel MakeDefaultDebugPixel()
{
  return (ReSTIRDebugPixel)0;
}

void WriteDebugPixel(uint pixelIndex,
                     ReSTIRPTReservoir reservoir,
                     float shiftJacobian,
                     float reuseCount,
                     uint temporalStatus,
                     uint spatialStatus)
{
  ReSTIRDebugPixel debugPixel = MakeDefaultDebugPixel();
  debugPixel.targetPdf        = reservoir.targetPdf;
  debugPixel.reservoirWeight  = reservoir.weight;
  debugPixel.shiftJacobian    = shiftJacobian;
  debugPixel.reuseCount       = reuseCount;
  debugPixel.candidateKind    = reservoir.candidateKind;
  debugPixel.reservoirAge     = reservoir.age;
  debugPixel.temporalStatus   = temporalStatus;
  debugPixel.spatialStatus    = spatialStatus;
  debugBuffer[pixelIndex]     = debugPixel;
}

float3 GetCandidateKindDebugColor(uint candidateKind)
{
  if(candidateKind == uint(ReSTIRPTCandidateKind::eReSTIRPTCandidateKindBroad))
    return float3(0.95, 0.65, 0.15);
  if(candidateKind == uint(ReSTIRPTCandidateKind::eReSTIRPTCandidateKindReflection))
    return float3(0.15, 0.75, 0.95);
  if(candidateKind == uint(ReSTIRPTCandidateKind::eReSTIRPTCandidateKindGlass))
    return float3(0.35, 0.95, 0.55);
  return float3(0.95, 0.25, 0.75);
}

float3 GetShiftStatusDebugColor(uint status)
{
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted))
    return float3(0.15, 0.9, 0.25);
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory))
    return float3(0.15, 0.45, 0.95);
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface))
    return float3(0.95, 0.3, 0.2);
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedDirection))
    return float3(0.95, 0.75, 0.2);
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedVisibility))
    return float3(0.85, 0.2, 0.85);
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedTargetPdf))
    return float3(0.85, 0.85, 0.2);
  if(status == uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedHistoryAge))
    return float3(0.5, 0.5, 0.5);
  return float3(0.0);
}

float3 VisualizeReSTIRPTDebug(uint pixelIndex, float3 fallbackColor)
{
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDisabled))
  {
    return fallbackColor;
  }

  const ReSTIRDebugPixel debugPixel = debugBuffer[pixelIndex];
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewCandidateKind))
  {
    return GetCandidateKindDebugColor(debugPixel.candidateKind);
  }
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewTargetPdf))
  {
    const float value = saturate(log2(debugPixel.targetPdf + 1.0) / 8.0);
    return float3(value, value, value);
  }
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewReservoirWeight))
  {
    const float value = saturate(debugPixel.reservoirWeight * 0.25);
    return float3(value, value, value);
  }
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewReservoirAge))
  {
    const float value = saturate(float(debugPixel.reservoirAge) / max(float(pushConst.temporalResampling.maxHistoryLength), 1.0));
    return lerp(float3(0.05, 0.1, 0.2), float3(0.95, 0.85, 0.25), value);
  }
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewTemporalStatus))
  {
    return GetShiftStatusDebugColor(debugPixel.temporalStatus);
  }
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewSpatialStatus))
  {
    return GetShiftStatusDebugColor(debugPixel.spatialStatus);
  }
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewShiftJacobian))
  {
    const float value = saturate(abs(log2(max(debugPixel.shiftJacobian, 1.0e-5))) / 6.0);
    return debugPixel.shiftJacobian >= 1.0 ? lerp(float3(0.1, 0.1, 0.1), float3(0.15, 0.95, 0.25), value)
                                           : lerp(float3(0.1, 0.1, 0.1), float3(0.95, 0.25, 0.2), value);
  }

  const float reuse = saturate(debugPixel.reuseCount / 8.0);
  return lerp(float3(0.05, 0.05, 0.08), float3(0.15, 0.8, 0.95), reuse);
}

#endif
