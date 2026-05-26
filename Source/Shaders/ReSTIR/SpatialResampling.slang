#include "ShaderIncludes/ReSTIR/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/Debug.h.slang"
#include "ReSTIR/SpatialResampling.hlsli"

// Compute pass after temporal reuse. Each pixel samples nearby reservoirs in
// screen space and combines only neighbors that pass surface/material tests.
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
  const uint2 pixelPosition = dispatchThreadID.xy;
  const uint2 viewportSize  = uint2(pushConst.sceneInfoAddress[0].viewportSize);
  if(any(pixelPosition >= viewportSize))
  {
    return;
  }

  const uint pixelIndex = pixelPosition.y * viewportSize.x + pixelPosition.x;
  const DISurface surface = currentSurfaceBuffer[pixelIndex];
  // Spatial reuse reads whichever reservoir array the CPU selected for this frame.
  const ReSTIRDIReservoir centerReservoir =
      LoadDIReservoir(diParams.reservoirBufferParams,
                      PixelPosToReservoirPos(pixelPosition),
                      diParams.bufferIndices.spatialResamplingInputBufferIndex);

  if(!DIIsSurfaceValid(surface))
  {
    // Miss/background pixels cannot provide a receiver for neighbor reservoirs.
    const ReSTIRDIReservoir emptyReservoir = EmptyDIReservoir();
    StoreDIReservoir(emptyReservoir, diParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition),
                     diParams.bufferIndices.spatialResamplingOutputBufferIndex);
    if(IsReSTIRDebugEnabled())
    {
      WriteDIDebugPixel(pixelIndex, emptyReservoir, 1.0, 0.0, debugBuffer[pixelIndex].temporalStatus,
                        uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface));
    }
    return;
  }

  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, diParams.runtimeParams.frameIndex, 3u);
  // The include handles neighbor selection, material checks, and normalization.
  const ReSTIRDIReservoir spatialReservoir =
      ReSTIRDIRunSpatialResampling(pixelPosition, surface, centerReservoir, rng, diParams.runtimeParams,
                                   diParams.reservoirBufferParams, diParams.bufferIndices.spatialResamplingInputBufferIndex,
                                   diParams.spatialResampling);

  StoreDIReservoir(spatialReservoir, diParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition),
                   diParams.bufferIndices.spatialResamplingOutputBufferIndex);

  // Debug status is inferred from whether a neighbor became the selected sample.
  const bool selectedNeighbor = spatialReservoir.lightData != centerReservoir.lightData || spatialReservoir.uvData != centerReservoir.uvData
                                || spatialReservoir.age != centerReservoir.age;
  const uint spatialStatus = selectedNeighbor ? uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted)
                                              : uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone);
  if(IsReSTIRDebugEnabled())
  {
    WriteDIDebugPixel(pixelIndex, spatialReservoir, 1.0, spatialReservoir.M, debugBuffer[pixelIndex].temporalStatus, spatialStatus);
  }
}
