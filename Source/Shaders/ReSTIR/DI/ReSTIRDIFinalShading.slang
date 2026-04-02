#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "ShaderIncludes/ReSTIR/DI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/Common/Common.h.slang"
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Continuation.h.slang"
#include "ShaderIncludes/ReSTIR/DI/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/DI/Debug.h.slang"

bool TraceHybridDIContinuation(SurfaceData surface, inout PathPayload payload)
{
  if(pushConst.continuationMaxBounces == 0)
  {
    return false;
  }

  const float3 viewDir = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surface.worldPosition);
  RayDesc bounceRay;
  bool    isTransmissionEvent = false;
  if(!BeginContinuationRay(payload, surface, viewDir, bounceRay, isTransmissionEvent))
  {
    return false;
  }

  payload.depth = 1;
  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
  return true;
}

void ContinueHybridDIPath(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  const SurfaceData   surface   = LoadSurfaceData(attr);
  const float3        viewDir   = -WorldRayDirection();
  ContinuePathFromSurface(payload, surface, viewDir, pushConst.continuationMaxBounces, true);
}

[shader("raygeneration")]
void rgenMain()
{
  const uint2 pixelPosition = DispatchRaysIndex().xy;
  const uint2 viewportSize  = uint2(pushConst.sceneInfoAddress[0].viewportSize);
  if(any(pixelPosition >= viewportSize))
  {
    return;
  }

  const uint pixelIndex = pixelPosition.y * viewportSize.x + pixelPosition.x;
  const DISurface surface = currentSurfaceBuffer[pixelIndex];
  const ReSTIRDIReservoir reservoir = LoadDIReservoir(diParams.reservoirBufferParams,
                                                      PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                                                      diParams.bufferIndices.shadingInputBufferIndex);

  float3 resolvedRadiance = ResolveReservoirDirectLighting(pixelPosition, surface, reservoir, true);
  if(DIIsSurfaceValid(surface) && pushConst.continuationMaxBounces > 0)
  {
    PathPayload payload;
    payload                          = (PathPayload)0;
    payload.radiance                 = resolvedRadiance;
    payload.throughput               = float3(1.0);
    payload.mediumAttenuation        = float3(0.0);
    payload.mediumThickness          = 0.0;
    payload.lastSurfacePosition      = surface.worldPosition;
    payload.lastSurfaceNormal        = surface.shadingNormal;
    payload.lastDirectLightBsdfPdf   = 0.0;
    payload.seed                     = xxhash32(uint3(pixelPosition, diParams.runtimeParams.frameIndex));
    payload.depth                    = 0;
    payload.mediumActive             = 0;
    payload.lastEnvironmentNeeActive = 0;
    payload.hasLastBsdfSample        = 0;

    const SurfaceData unpackedSurface = UnpackDIPrimarySurface(surface);
    TraceHybridDIContinuation(unpackedSurface, payload);
    resolvedRadiance = payload.radiance;
  }

  resolvedRadiance = SanitizeRadiance(resolvedRadiance);
  resolvedRadiance = VisualizeReSTIRDIDebug(pixelIndex, resolvedRadiance);
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDisabled)
     && (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagAccumulate)) != 0 && pushConst.accumulatedFrames > 0)
  {
    const float3 previousAverage = accumulationImage[int2(pixelPosition)].xyz;
    const float historyWeight    = float(pushConst.accumulatedFrames);
    resolvedRadiance = (previousAverage * historyWeight + resolvedRadiance) / (historyWeight + 1.0);
  }

  accumulationImage[int2(pixelPosition)] = float4(resolvedRadiance, 1.0);
  outImage[int2(pixelPosition)]          = float4(resolvedRadiance, 1.0);
}

[shader("miss")]
void rmissMain(inout PathPayload payload)
{
  AccumulateContinuationMissRadiance(payload);
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload)
{
  payload.visible = 1;
}

[shader("anyhit")]
void rahitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  if(IsMaskedSurfaceHit(attr))
  {
    IgnoreHit();
  }
}

[shader("anyhit")]
void shadowAnyHitMain(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  if(IsMaskedSurfaceHit(attr))
  {
    IgnoreHit();
  }
}

[shader("closesthit")]
void rchitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  ContinueHybridDIPath(payload, attr);
}
