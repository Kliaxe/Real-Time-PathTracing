#include "ShaderIncludes/ReSTIR/DI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/DI/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/Common/Common.h.slang"
#include "ReSTIR/DI/TemporalResampling.hlsli"

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
  const ReSTIRDIReservoir currentReservoir =
      LoadDIReservoir(diParams.reservoirBufferParams,
                      PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                      diParams.bufferIndices.initialSamplingOutputBufferIndex);

  if(!DIIsSurfaceValid(surface))
  {
    StoreDIReservoir(EmptyDIReservoir(), diParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                     diParams.bufferIndices.temporalResamplingOutputBufferIndex);
    return;
  }

  const GltfSceneInfo sceneInfo         = pushConst.sceneInfoAddress[0];
  const float3 previousPixelAndDepth    = ReprojectWorldPositionToPixel(surface.worldPosition, sceneInfo);
  if(previousPixelAndDepth.x < 0.0 || previousPixelAndDepth.y < 0.0)
  {
    StoreDIReservoir(currentReservoir, diParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                     diParams.bufferIndices.temporalResamplingOutputBufferIndex);
    return;
  }

  const float2 currentPixel      = float2(pixelPosition);
  const float  previousDepth     = length(surface.worldPosition - sceneInfo.prevCameraPosition);
  const float3 screenSpaceMotion = float3(previousPixelAndDepth.xy - currentPixel, previousDepth - surface.linearDepth);

  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, diParams.runtimeParams.frameIndex, 2u);
  int2 temporalSamplePixelPos;
  DILightSample selectedLightSample = DIEmptyLightSample();
  const ReSTIRDIReservoir temporalReservoir =
      ReSTIRDIRunTemporalResampling(pixelPosition, surface, currentReservoir, rng, diParams.runtimeParams,
                                    diParams.reservoirBufferParams, screenSpaceMotion,
                                    diParams.bufferIndices.temporalResamplingInputBufferIndex, diParams.temporalResampling,
                                    temporalSamplePixelPos, selectedLightSample);

  StoreDIReservoir(temporalReservoir, diParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                   diParams.bufferIndices.temporalResamplingOutputBufferIndex);
}
