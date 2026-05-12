#include "ShaderIncludes/ReSTIR/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/Debug.h.slang"
#include "ReSTIR/SpatialResampling.hlsli"

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
  const ReSTIRDIReservoir centerReservoir =
      LoadDIReservoir(diParams.reservoirBufferParams,
                      PixelPosToReservoirPos(pixelPosition),
                      diParams.bufferIndices.spatialResamplingInputBufferIndex);

  if(!DIIsSurfaceValid(surface))
  {
    const ReSTIRDIReservoir emptyReservoir = EmptyDIReservoir();
    StoreDIReservoir(emptyReservoir, diParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition),
                     diParams.bufferIndices.spatialResamplingOutputBufferIndex);
    WriteDIDebugPixel(pixelIndex, emptyReservoir, 1.0, 0.0, debugBuffer[pixelIndex].temporalStatus,
                      uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface));
    return;
  }

  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, diParams.runtimeParams.frameIndex, 3u);
  DILightSample selectedLightSample = DIEmptyLightSample();
  const ReSTIRDIReservoir spatialReservoir =
      ReSTIRDIRunSpatialResampling(pixelPosition, surface, centerReservoir, rng, diParams.runtimeParams,
                                   diParams.reservoirBufferParams, diParams.bufferIndices.spatialResamplingInputBufferIndex,
                                   diParams.spatialResampling, selectedLightSample);

  StoreDIReservoir(spatialReservoir, diParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition),
                   diParams.bufferIndices.spatialResamplingOutputBufferIndex);

  const bool selectedNeighbor = spatialReservoir.lightData != centerReservoir.lightData || spatialReservoir.uvData != centerReservoir.uvData
                                || spatialReservoir.age != centerReservoir.age;
  const uint spatialStatus = selectedNeighbor ? uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted)
                                              : uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone);
  WriteDIDebugPixel(pixelIndex, spatialReservoir, 1.0, spatialReservoir.M, debugBuffer[pixelIndex].temporalStatus, spatialStatus);
}
