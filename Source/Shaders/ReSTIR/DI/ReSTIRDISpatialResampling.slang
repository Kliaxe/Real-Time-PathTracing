#include "ShaderIncludes/ReSTIR/DI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/DI/Lighting.h.slang"
#include "ReSTIR/DI/SpatialResampling.hlsli"

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
                      PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                      diParams.bufferIndices.spatialResamplingInputBufferIndex);

  if(!DIIsSurfaceValid(surface))
  {
    StoreDIReservoir(EmptyDIReservoir(), diParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                     diParams.bufferIndices.spatialResamplingOutputBufferIndex);
    return;
  }

  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, diParams.runtimeParams.frameIndex, 3u);
  DILightSample selectedLightSample = DIEmptyLightSample();
  const ReSTIRDIReservoir spatialReservoir =
      ReSTIRDIRunSpatialResampling(pixelPosition, surface, centerReservoir, rng, diParams.runtimeParams,
                                   diParams.reservoirBufferParams, diParams.bufferIndices.spatialResamplingInputBufferIndex,
                                   diParams.spatialResampling, selectedLightSample);

  StoreDIReservoir(spatialReservoir, diParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                   diParams.bufferIndices.spatialResamplingOutputBufferIndex);
}
