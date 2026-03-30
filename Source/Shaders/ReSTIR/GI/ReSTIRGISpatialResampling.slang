#define RESTIR_GI_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_BASIC
#define RESTIR_GI_ENABLE_FINAL_VISIBILITY 0
#include "ShaderIncludes/ReSTIR/GI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Lighting.h.slang"
#include "ReSTIR/Common/Utils/RandomSamplerState.hlsli"
#include "ReSTIR/GI/SpatialResampling.hlsli"

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
  const GISurface surface = currentSurfaceBuffer[pixelIndex];
  const ReSTIRGIReservoir centerReservoir =
      LoadGIReservoir(giParams.reservoirBufferParams, PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                      giParams.bufferIndices.spatialResamplingInputBufferIndex);

  if(!GIIsSurfaceValid(surface))
  {
    StoreGIReservoir(EmptyGIReservoir(), giParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                     giParams.bufferIndices.spatialResamplingOutputBufferIndex);
    return;
  }

  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, giParams.runtimeParams.frameIndex, 5u);
  const ReSTIRGIReservoir spatialReservoir =
      RunGISpatialResampling(pixelPosition, surface, giParams.bufferIndices.spatialResamplingInputBufferIndex, centerReservoir, rng,
                             giParams.runtimeParams, giParams.reservoirBufferParams, giParams.spatialResampling);

  StoreGIReservoir(spatialReservoir, giParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                   giParams.bufferIndices.spatialResamplingOutputBufferIndex);
}
