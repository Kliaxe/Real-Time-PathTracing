#define RESTIR_GI_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_BASIC
#define RESTIR_GI_ENABLE_FINAL_VISIBILITY 0
#include "ShaderIncludes/ReSTIR/GI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Debug.h.slang"
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
    const ReSTIRGIReservoir emptyReservoir = EmptyGIReservoir();
    StoreGIReservoir(emptyReservoir, giParams.reservoirBufferParams,
                     PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                     giParams.bufferIndices.spatialResamplingOutputBufferIndex);
    WriteGIDebugPixel(pixelIndex, surface, emptyReservoir, 1.0, 0.0, debugBuffer[pixelIndex].temporalStatus,
                      uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface));
    return;
  }

  ReSTIRRandomSamplerState rng = InitRandomSampler(pixelPosition, giParams.runtimeParams.frameIndex, 5u);
  const ReSTIRGIReservoir spatialReservoir =
      RunGISpatialResampling(pixelPosition, surface, giParams.bufferIndices.spatialResamplingInputBufferIndex, centerReservoir, rng,
                             giParams.runtimeParams, giParams.reservoirBufferParams, giParams.spatialResampling);

  StoreGIReservoir(spatialReservoir, giParams.reservoirBufferParams,
                   PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                   giParams.bufferIndices.spatialResamplingOutputBufferIndex);

  const bool selectedNeighbor = spatialReservoir.age != centerReservoir.age || spatialReservoir.sampleKind != centerReservoir.sampleKind
                                || any(spatialReservoir.samplePosition != centerReservoir.samplePosition)
                                || any(spatialReservoir.sampleDirection != centerReservoir.sampleDirection)
                                || any(spatialReservoir.sampleRadiance != centerReservoir.sampleRadiance);
  const uint spatialStatus = selectedNeighbor ? uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted)
                                              : uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone);
  WriteGIDebugPixel(pixelIndex, surface, spatialReservoir, 1.0, spatialReservoir.M, debugBuffer[pixelIndex].temporalStatus, spatialStatus);
}
