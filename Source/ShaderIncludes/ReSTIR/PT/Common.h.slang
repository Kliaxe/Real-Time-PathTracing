#ifndef RESTIR_PT_COMMON_H
#define RESTIR_PT_COMMON_H

#include "ShaderIncludes/ReSTIR/PT/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/Common/Common.h.slang"

bool IsReSTIRPTAccumulationEnabled()
{
  return (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagAccumulate)) != 0;
}

bool HasTemporalHistory()
{
  return (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagHasHistory)) != 0;
}

bool InitialPassWritesHistory()
{
  return (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagInitialWriteToHistory)) != 0;
}

bool TemporalPassWritesScratch()
{
  return (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagTemporalWriteToScratch)) != 0;
}

bool SpatialPassReadsScratch()
{
  return (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagSpatialReadFromScratch)) != 0;
}

uint GetReSTIRPTPathMaxBounceDepth()
{
  // The thesis baseline uses one bounce budget for both initial generation and
  // final replay.
  return pushConst.initialSampling.maxBounceDepth;
}

bool IsShiftVisibilityValidationEnabled()
{
  return pushConst.enableVisibilityValidation != 0u;
}

ReSTIRPTReservoir LoadSpatialInputReservoir(uint pixelIndex)
{
  // Spatial reuse reads either the temporal scratch buffer or the initial
  // reservoir buffer depending on which passes ran earlier this frame.
  return SpatialPassReadsScratch() ? scratchReservoirBuffer[pixelIndex] : initialReservoirBuffer[pixelIndex];
}

void StoreInitialOutput(uint pixelIndex, ReSTIRPTReservoir reservoir)
{
  // In no-resampling mode, the initial pass writes straight into history so the
  // final replay pass can keep one fixed read path.
  if(InitialPassWritesHistory())
  {
    currentHistoryReservoirBuffer[pixelIndex] = reservoir;
  }
  else
  {
    initialReservoirBuffer[pixelIndex] = reservoir;
  }
}

void StoreTemporalOutput(uint pixelIndex, ReSTIRPTReservoir reservoir)
{
  // Temporal writes scratch only when a later spatial pass still needs the
  // intermediate result this frame.
  if(TemporalPassWritesScratch())
  {
    scratchReservoirBuffer[pixelIndex] = reservoir;
  }
  else
  {
    currentHistoryReservoirBuffer[pixelIndex] = reservoir;
  }
}

#endif
