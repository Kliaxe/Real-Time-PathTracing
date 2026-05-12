#include "ShaderIncludes/ReSTIR/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/Debug.h.slang"
#include "ShaderIncludes/ReSTIR/Common.h.slang"
#include "ReSTIR/TemporalResampling.hlsli"

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
                      PixelPosToReservoirPos(pixelPosition),
                      diParams.bufferIndices.initialSamplingOutputBufferIndex);

  if(!DIIsSurfaceValid(surface))
  {
    const ReSTIRDIReservoir emptyReservoir = EmptyDIReservoir();
    StoreDIReservoir(emptyReservoir, diParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition),
                     diParams.bufferIndices.temporalResamplingOutputBufferIndex);
    WriteDIDebugPixel(pixelIndex, emptyReservoir, 1.0, 0.0, uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface),
                      debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const float2 pixelCenter      = float2(pixelPosition) + 0.5;
  float3 temporalMotionVector   = float3(0.0, 0.0, 0.0);
  if(!TryComputeReSTIRTemporalMotionVector(pixelCenter, surface.worldPosition, surface.linearDepth, sceneInfo, temporalMotionVector))
  {
    StoreDIReservoir(currentReservoir, diParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition),
                     diParams.bufferIndices.temporalResamplingOutputBufferIndex);
    WriteDIDebugPixel(pixelIndex, currentReservoir, 1.0, currentReservoir.M, uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory),
                      debugBuffer[pixelIndex].spatialStatus);
    return;
  }

  const float3 screenSpaceMotion = ConvertMotionVectorToPixelSpace(pixelCenter, temporalMotionVector, sceneInfo);

  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, diParams.runtimeParams.frameIndex, 2u);
  int2 temporalSamplePixelPos;
  DILightSample selectedLightSample = DIEmptyLightSample();
  const ReSTIRDIReservoir temporalReservoir =
      ReSTIRDIRunTemporalResampling(pixelPosition, surface, currentReservoir, rng, diParams.runtimeParams,
                                    diParams.reservoirBufferParams, screenSpaceMotion,
                                    diParams.bufferIndices.temporalResamplingInputBufferIndex, diParams.temporalResampling,
                                    temporalSamplePixelPos, selectedLightSample);

  StoreDIReservoir(temporalReservoir, diParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition),
                   diParams.bufferIndices.temporalResamplingOutputBufferIndex);

  uint temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone);
  if(diParams.runtimeParams.frameIndex == 0u)
  {
    temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory);
  }
  else if(temporalReservoir.age > currentReservoir.age)
  {
    temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
  }

  WriteDIDebugPixel(pixelIndex, temporalReservoir, 1.0, temporalReservoir.M, temporalStatus, debugBuffer[pixelIndex].spatialStatus);
}
