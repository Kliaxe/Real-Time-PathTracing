#include "ShaderIncludes/ReSTIR/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/Debug.h.slang"
#include "ShaderIncludes/ReSTIR/Common.h.slang"
#include "ReSTIR/TemporalResampling.hlsli"

// Compute pass between initial sampling and spatial reuse. Each pixel
// reprojects its current primary surface into the previous surface buffer and
// combines the previous reservoir when the surfaces still match.
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
  // Temporal reuse starts from the fresh reservoir produced this frame.
  const ReSTIRDIReservoir currentReservoir =
      LoadDIReservoir(diParams.reservoirBufferParams,
                      PixelPosToReservoirPos(pixelPosition),
                      diParams.bufferIndices.initialSamplingOutputBufferIndex);

  if(!DIIsSurfaceValid(surface))
  {
    // Miss/background pixels should not keep stale temporal reservoirs alive.
    const ReSTIRDIReservoir emptyReservoir = EmptyDIReservoir();
    StoreDIReservoir(emptyReservoir, diParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition),
                     diParams.bufferIndices.temporalResamplingOutputBufferIndex);
    if(IsReSTIRDebugEnabled())
    {
      WriteDIDebugPixel(pixelIndex, emptyReservoir, 1.0, 0.0, uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone),
                        debugBuffer[pixelIndex].spatialStatus);
    }
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const float2 pixelCenter      = float2(pixelPosition) + 0.5;
  float3 temporalMotionVector   = float3(0.0, 0.0, 0.0);
  // If reprojection fails, keep only the current-frame initial reservoir.
  if(!TryComputeReSTIRTemporalMotionVector(pixelCenter, surface.worldPosition, surface.linearDepth, sceneInfo, temporalMotionVector))
  {
    StoreDIReservoir(currentReservoir, diParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition),
                     diParams.bufferIndices.temporalResamplingOutputBufferIndex);
    if(IsReSTIRDebugEnabled())
    {
      WriteDIDebugPixel(pixelIndex, currentReservoir, 1.0, currentReservoir.M, uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory),
                        debugBuffer[pixelIndex].spatialStatus);
    }
    return;
  }

  const float3 screenSpaceMotion = ConvertMotionVectorToPixelSpace(pixelCenter, temporalMotionVector, sceneInfo);

  // ReSTIRDIRunTemporalResampling owns previous-pixel lookup and surface validation.
  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, diParams.runtimeParams.frameIndex, 2u);
  int2 temporalSamplePixelPos;
  uint temporalStatus;
  const ReSTIRDIReservoir temporalReservoir =
      ReSTIRDIRunTemporalResampling(pixelPosition, surface, currentReservoir, rng, diParams.reservoirBufferParams, screenSpaceMotion,
                                    diParams.bufferIndices.temporalResamplingInputBufferIndex, diParams.temporalResampling,
                                    temporalSamplePixelPos, temporalStatus);

  StoreDIReservoir(temporalReservoir, diParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition),
                   diParams.bufferIndices.temporalResamplingOutputBufferIndex);

  if(diParams.runtimeParams.frameIndex == 0u)
  {
    // First frame has no meaningful previous reservoir even if buffers contain data.
    temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedNoHistory);
  }

  if(IsReSTIRDebugEnabled())
  {
    WriteDIDebugPixel(pixelIndex, temporalReservoir, 1.0, temporalReservoir.M, temporalStatus, debugBuffer[pixelIndex].spatialStatus);
  }
}
