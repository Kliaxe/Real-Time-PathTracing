#include "ShaderIncludes/ReSTIR/DI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/DI/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/Common/Common.h.slang"
#define RESTIR_ENABLE_BOILING_FILTER
#define RESTIR_BOILING_FILTER_GROUP_SIZE 8
#include "ReSTIR/Common/BoilingFilter.hlsli"
#include "ReSTIR/DI/TemporalResampling.hlsli"

ReSTIRDIReservoir ApplyDIBoilingFilter(uint2 localIndex, ReSTIRDIReservoir reservoir)
{
  if(diParams.boilingFilter.enableBoilingFilter == 0u || diParams.boilingFilter.boilingFilterStrength <= 0.0 || !IsValidDIReservoir(reservoir))
  {
    return reservoir;
  }

  if(ReSTIRBoilingFilterInternal(localIndex, diParams.boilingFilter.boilingFilterStrength, reservoir.weightSum))
  {
    return EmptyDIReservoir();
  }

  return reservoir;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID)
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
    const ReSTIRDIReservoir filteredReservoir = ApplyDIBoilingFilter(groupThreadID.xy, currentReservoir);
    StoreDIReservoir(filteredReservoir, diParams.reservoirBufferParams,
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
  const ReSTIRDIReservoir filteredReservoir = ApplyDIBoilingFilter(groupThreadID.xy, temporalReservoir);

  StoreDIReservoir(filteredReservoir, diParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                   diParams.bufferIndices.temporalResamplingOutputBufferIndex);
}
