#define RESTIR_GI_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_BASIC
#define RESTIR_GI_ENABLE_FINAL_VISIBILITY 0
#include "ShaderIncludes/ReSTIR/GI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Debug.h.slang"
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
    const ReSTIRGIReservoir emptyReservoir = EmptyGIReservoir();
    StoreGIReservoir(emptyReservoir, giParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                     giParams.bufferIndices.temporalResamplingOutputBufferIndex);
    WriteGIDebugPixel(pixelIndex, surface, emptyReservoir, 1.0, 0.0, uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface),
                      debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const float2 pixelCenter      = float2(pixelPosition) + 0.5;
  float3 temporalMotionVector   = float3(0.0, 0.0, 0.0);
  if(!TryComputeReSTIRTemporalMotionVector(pixelCenter, surface.worldPosition, surface.linearDepth, sceneInfo, temporalMotionVector))
  {
    const ReSTIRGIReservoir filteredReservoir = ApplyGIBoilingFilter(groupThreadID.xy, currentReservoir);
    StoreGIReservoir(filteredReservoir, giParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                     giParams.bufferIndices.temporalResamplingOutputBufferIndex);
    WriteGIDebugPixel(pixelIndex, surface, filteredReservoir, 1.0, filteredReservoir.M,
                      uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory), debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  const float3 screenSpaceMotion = ConvertMotionVectorToPixelSpace(pixelCenter, temporalMotionVector, sceneInfo);

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

  uint temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone);
  if(giParams.runtimeParams.frameIndex == 0u)
  {
    temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory);
  }
  else if(filteredReservoir.age > currentReservoir.age)
  {
    temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
  }

  WriteGIDebugPixel(pixelIndex, surface, filteredReservoir, 1.0, filteredReservoir.M, temporalStatus, debugBuffer[pixelIndex].spatialStatus);
}
