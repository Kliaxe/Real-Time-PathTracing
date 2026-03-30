#define RESTIR_GI_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_BASIC
#define RESTIR_GI_ENABLE_FINAL_VISIBILITY 0
#include "ShaderIncludes/ReSTIR/GI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Lighting.h.slang"
#include "ReSTIR/Common/Utils/RandomSamplerState.hlsli"
#include "ReSTIR/GI/TemporalResampling.hlsli"

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
  const ReSTIRGIReservoir currentReservoir =
      LoadGIReservoir(giParams.reservoirBufferParams, PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                      giParams.bufferIndices.initialSamplingOutputBufferIndex);

  if(!GIIsSurfaceValid(surface))
  {
    StoreGIReservoir(EmptyGIReservoir(), giParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                     giParams.bufferIndices.temporalResamplingOutputBufferIndex);
    return;
  }

  const GltfSceneInfo sceneInfo      = pushConst.sceneInfoAddress[0];
  const float3 previousPixelAndDepth = ReprojectWorldPositionToPixel(surface.worldPosition, sceneInfo);
  if(previousPixelAndDepth.x < 0.0 || previousPixelAndDepth.y < 0.0)
  {
    StoreGIReservoir(currentReservoir, giParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                     giParams.bufferIndices.temporalResamplingOutputBufferIndex);
    return;
  }

  const float2 currentPixel      = float2(pixelPosition);
  const float  previousDepth     = length(surface.worldPosition - sceneInfo.prevCameraPosition);
  const float3 screenSpaceMotion = float3(previousPixelAndDepth.xy - currentPixel, previousDepth - surface.linearDepth);

  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, giParams.runtimeParams.frameIndex, 4u);
  const ReSTIRGIReservoir temporalReservoir =
      RunGITemporalResampling(pixelPosition, surface, screenSpaceMotion, giParams.bufferIndices.temporalResamplingInputBufferIndex,
                              currentReservoir, rng, giParams.runtimeParams, giParams.reservoirBufferParams, giParams.temporalResampling);

  StoreGIReservoir(temporalReservoir, giParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                   giParams.bufferIndices.temporalResamplingOutputBufferIndex);
}
