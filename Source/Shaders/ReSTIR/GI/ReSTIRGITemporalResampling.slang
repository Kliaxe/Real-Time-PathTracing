#define RESTIR_GI_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_BASIC
#define RESTIR_GI_ENABLE_FINAL_VISIBILITY 0
#include "ShaderIncludes/ReSTIR/GI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Lighting.h.slang"
#define RESTIR_ENABLE_BOILING_FILTER
#define RESTIR_BOILING_FILTER_GROUP_SIZE 8
#include "ReSTIR/Common/BoilingFilter.hlsli"
#include "ReSTIR/Common/Utils/RandomSamplerState.hlsli"
#include "ReSTIR/GI/TemporalResampling.hlsli"

ReSTIRGIReservoir ApplyGIBoilingFilter(uint2 localIndex, ReSTIRGIReservoir reservoir)
{
  if(giParams.boilingFilter.enableBoilingFilter == 0u || giParams.boilingFilter.boilingFilterStrength <= 0.0 || !IsValidGIReservoir(reservoir))
  {
    return reservoir;
  }

  const float reservoirWeight = ReSTIRLuminance(reservoir.sampleRadiance) * reservoir.weightSum;
  if(ReSTIRBoilingFilterInternal(localIndex, giParams.boilingFilter.boilingFilterStrength, reservoirWeight))
  {
    return EmptyGIReservoir();
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
    const ReSTIRGIReservoir filteredReservoir = ApplyGIBoilingFilter(groupThreadID.xy, currentReservoir);
    StoreGIReservoir(filteredReservoir, giParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                     giParams.bufferIndices.temporalResamplingOutputBufferIndex);
    return;
  }

  const float2 currentPixel      = float2(pixelPosition);
  const float  previousDepth     = length(surface.worldPosition - sceneInfo.prevCameraPosition);
  const float3 screenSpaceMotion = float3(previousPixelAndDepth.xy - currentPixel, previousDepth - surface.linearDepth);

  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, giParams.runtimeParams.frameIndex, 4u);
  ReSTIRGITemporalResamplingParameters temporalParams = giParams.temporalResampling;
  temporalParams.maxReservoirAge =
      max(1u, uint(float(giParams.temporalResampling.maxReservoirAge) * (0.5 + GetNextRandom(rng) * 0.5)));
  const ReSTIRGIReservoir temporalReservoir =
      RunGITemporalResampling(pixelPosition, surface, screenSpaceMotion, giParams.bufferIndices.temporalResamplingInputBufferIndex,
                              currentReservoir, rng, giParams.runtimeParams, giParams.reservoirBufferParams, temporalParams);
  const ReSTIRGIReservoir filteredReservoir = ApplyGIBoilingFilter(groupThreadID.xy, temporalReservoir);

  StoreGIReservoir(filteredReservoir, giParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                   giParams.bufferIndices.temporalResamplingOutputBufferIndex);
}
