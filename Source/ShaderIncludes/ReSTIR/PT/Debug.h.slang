#ifndef RESTIR_PT_DEBUG_H
#define RESTIR_PT_DEBUG_H

#include "ShaderIncludes/ReSTIR/Common/Debug.h.slang"
#include "ShaderIncludes/ReSTIR/PT/Shift.h.slang"

void WriteDebugPixel(uint pixelIndex,
                     ReSTIRPTReservoir reservoir,
                     float shiftJacobian,
                     float reuseCount,
                     uint temporalStatus,
                     uint spatialStatus)
{
  WriteReSTIRDebugPixel(pixelIndex, reservoir.targetPdf, reservoir.weight, shiftJacobian, reuseCount, reservoir.candidateKind, reservoir.age,
                        temporalStatus, spatialStatus);
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

float3 VisualizeReSTIRPTDebug(uint pixelIndex, float3 fallbackColor)
{
  const ReSTIRDebugPixel debugPixel = debugBuffer[pixelIndex];
  return VisualizeReSTIRCommonDebugPixel(debugPixel, pushConst.debugView, float(pushConst.temporalResampling.maxHistoryLength), fallbackColor,
                                         GetCandidateKindDebugColor(debugPixel.candidateKind));
}

#endif
