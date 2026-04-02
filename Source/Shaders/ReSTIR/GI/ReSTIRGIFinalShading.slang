#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Debug.h.slang"
#include "ShaderIncludes/PathTracing/Continuation.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"

bool BeginGlossyGIContinuationRay(inout PathPayload payload, SurfaceData surface, float3 viewDir, out RayDesc bounceRay)
{
  bounceRay = (RayDesc)0;

  float3 bounceDir           = float3(0.0);
  float3 bsdfOverPdf         = float3(0.0);
  float  directLightBsdfPdf  = 0.0;
  bool   isTransmissionEvent = false;
  uint   sampledLobeKind     = 0u;
  if(!SampleSurfaceBsdf(surface, viewDir, payload.seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
  {
    return false;
  }

  // The GI reservoir already carries the broad first-bounce indirect term.
  // The hybrid continuation only owns glossy / glass / clearcoat transport.
  if(sampledLobeKind == 0u)
  {
    return false;
  }

  const float geometricDot = dot(surface.geometricNormal, bounceDir);
  if(abs(geometricDot) <= 1.0e-6)
  {
    return false;
  }

  const float3 nextThroughput = payload.throughput * max(bsdfOverPdf, float3(0.0));
  if(SafeMax3(nextThroughput) <= 0.0)
  {
    return false;
  }

  PrepareContinuationPayloadForBounce(payload, surface, bounceDir, nextThroughput, directLightBsdfPdf, isTransmissionEvent);

  bounceRay.Origin    = OffsetRay(surface.worldPosition, SelectOffsetNormal(surface.geometricNormal, bounceDir));
  bounceRay.Direction = bounceDir;
  bounceRay.TMin      = 0.001;
  bounceRay.TMax      = kRayTMax;
  return true;
}

bool TraceHybridGIContinuation(GISurface primarySurface, inout PathPayload payload)
{
  if(pushConst.continuationMaxBounces == 0u || !GIIsSurfaceValid(primarySurface))
  {
    return false;
  }

  const SurfaceData surface = UnpackGIPrimarySurface(primarySurface);
  const float3      viewDir = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surface.worldPosition);

  RayDesc bounceRay;
  if(!BeginGlossyGIContinuationRay(payload, surface, viewDir, bounceRay))
  {
    return false;
  }

  payload.depth = 1u;
  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
  return true;
}

void ContinueHybridGIPath(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  const SurfaceData surface = LoadSurfaceData(attr);
  const float3      viewDir = -WorldRayDirection();
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
  const GISurface surface = currentSurfaceBuffer[pixelIndex];
  const ReSTIRGIReservoir reservoir =
      LoadGIReservoir(giParams.reservoirBufferParams, PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                      giParams.bufferIndices.shadingInputBufferIndex);
  const ReSTIRGIReservoir initialReservoir = giInitialSampleBuffer[pixelIndex];

  float3 resolvedRadiance = GIIsSurfaceValid(surface) ? surface.baseRadiance : ReconstructGIMissRadiance(pixelPosition);
  if(GIIsSurfaceValid(surface))
  {
    float3 reusedContribution = float3(0.0);
    float3 reusedBsdf         = float3(0.0);
    const bool hasReusedContribution = GIEvaluateReservoirContribution(surface, reservoir, true, reusedContribution, reusedBsdf);

    float3 initialContribution = float3(0.0);
    float3 initialBsdf         = float3(0.0);
    const bool hasInitialContribution = GIEvaluateReservoirContribution(surface, initialReservoir, false, initialContribution, initialBsdf);

    if(giParams.shading.enableFinalMIS != 0u && hasReusedContribution && hasInitialContribution)
    {
      const GISurface roughenedSurface = GIMakeFinalMISRoughenedSurface(surface);

      float roughReusedPdf = 0.0;
      float roughInitialPdf = 0.0;
      const float3 roughReusedBsdf = GIEvaluateReservoirBsdf(roughenedSurface, reservoir, roughReusedPdf);
      const float3 roughInitialBsdf = GIEvaluateReservoirBsdf(roughenedSurface, initialReservoir, roughInitialPdf);

      const float reusedWeight  = 1.0 - GIGetFinalMISWeight(roughReusedBsdf, reusedBsdf);
      const float initialWeight = GIGetFinalMISWeight(roughInitialBsdf, initialBsdf);
      resolvedRadiance += reusedContribution * reusedWeight + initialContribution * initialWeight;
    }
    else if(hasReusedContribution)
    {
      resolvedRadiance += reusedContribution;
    }
    else if(hasInitialContribution)
    {
      resolvedRadiance += initialContribution;
    }
  }

  if(GIIsSurfaceValid(surface) && pushConst.continuationMaxBounces > 0u)
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
    payload.seed                     = xxhash32(uint3(pixelPosition, giParams.runtimeParams.frameIndex));
    payload.depth                    = 0u;
    payload.mediumActive             = 0u;
    payload.lastEnvironmentNeeActive = 0u;
    payload.hasLastBsdfSample        = 0u;
    TraceHybridGIContinuation(surface, payload);
    resolvedRadiance = payload.radiance;
  }

  resolvedRadiance = SanitizeRadiance(resolvedRadiance);
  resolvedRadiance = VisualizeReSTIRGIDebug(pixelIndex, resolvedRadiance);
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDisabled)
     && (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagAccumulate)) != 0u && pushConst.accumulatedFrames > 0u)
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
  payload.visible = 1u;
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
  ContinueHybridGIPath(payload, attr);
}
