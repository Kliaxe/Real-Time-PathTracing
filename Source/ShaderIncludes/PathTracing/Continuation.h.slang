#ifndef PATH_TRACING_CONTINUATION_H
#define PATH_TRACING_CONTINUATION_H

#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"

void AccumulateContinuationMissRadiance(inout PathPayload payload)
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const float3        envColor  = SampleEnvironment(sceneInfo, WorldRayDirection());

  float misWeight = 1.0;
  if(payload.hasLastBsdfSample != 0 && payload.lastEnvironmentNeeActive != 0)
  {
    const float lightPdf = EvaluateEnvironmentLightPdf(sceneInfo, payload.lastSurfaceNormal, WorldRayDirection());
    if(lightPdf > 0.0 && payload.lastDirectLightBsdfPdf > 0.0)
    {
      misWeight = MisMixWeight(payload.lastDirectLightBsdfPdf, lightPdf);
    }
  }

  float3 envThroughput = payload.throughput;
  if(payload.mediumActive != 0)
  {
    envThroughput *= EvaluateMediumTransmittance(payload.mediumAttenuation, payload.mediumThickness);
  }

  payload.radiance += envThroughput * envColor * misWeight;
}

void PrepareContinuationPayloadForBounce(inout PathPayload payload,
                                         SurfaceData surface,
                                         float3 bounceDir,
                                         float3 nextThroughput,
                                         float directLightBsdfPdf,
                                         bool isTransmissionEvent)
{
  payload.throughput = nextThroughput;
  if(isTransmissionEvent)
  {
    if(surface.isFrontFace != 0 && HasVolumeAttenuation(surface))
    {
      payload.mediumAttenuation = BuildAttenuationCoefficient(surface.attenuationColor, surface.attenuationDistance);
      payload.mediumThickness   = surface.volumeThickness;
      payload.mediumActive      = 1;
    }
    else if(surface.isFrontFace == 0)
    {
      payload.mediumAttenuation = float3(0.0);
      payload.mediumThickness   = 0.0;
      payload.mediumActive      = 0;
    }
  }

  payload.lastSurfacePosition    = surface.worldPosition;
  payload.lastSurfaceNormal      = surface.shadingNormal;
  payload.lastDirectLightBsdfPdf = directLightBsdfPdf;

  bool environmentNeeActive = false;
  if(isTransmissionEvent)
  {
    environmentNeeActive = CanSampleEnvironmentTransmissionExitLight(surface) && directLightBsdfPdf > 0.0;
  }
  else
  {
    environmentNeeActive = CanSampleEnvironmentReflectionDirectLight(surface) && directLightBsdfPdf > 0.0;
  }
  payload.lastEnvironmentNeeActive = environmentNeeActive ? 1u : 0u;
  payload.hasLastBsdfSample        = 1u;
}

bool BeginContinuationRay(inout PathPayload payload,
                          SurfaceData surface,
                          float3 viewDir,
                          out RayDesc bounceRay,
                          out bool isTransmissionEvent)
{
  bounceRay            = (RayDesc)0;
  isTransmissionEvent  = false;

  float3 bounceDir;
  float3 bsdfOverPdf        = float3(0.0);
  float  directLightBsdfPdf = 0.0;
  uint   sampledLobeKind    = 0u;
  if(!SampleSurfaceBsdf(surface, viewDir, payload.seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
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

  const float3 originNormal = SelectOffsetNormal(surface.geometricNormal, bounceDir);
  bounceRay.Origin          = OffsetRay(surface.worldPosition, originNormal);
  bounceRay.Direction       = bounceDir;
  bounceRay.TMin            = 0.001;
  bounceRay.TMax            = kRayTMax;
  return true;
}

bool ContinuePathFromSurface(inout PathPayload payload,
                             SurfaceData surface,
                             float3 viewDir,
                             uint maxBounces,
                             bool enableRussianRoulette)
{
  ApplyCurrentMediumAttenuation(payload, RayTCurrent());

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  float emissiveMisWeight = 1.0;
  if(payload.hasLastBsdfSample != 0 && SafeMax3(surface.emission) > 0.0)
  {
    const float lightPdf =
        EvaluateCurrentEmissiveHitPdf(sceneInfo, InstanceIndex(), PrimitiveIndex(), payload.lastSurfacePosition, surface.worldPosition);
    if(lightPdf > 0.0 && payload.lastDirectLightBsdfPdf > 0.0)
    {
      emissiveMisWeight = MisMixWeight(payload.lastDirectLightBsdfPdf, lightPdf);
    }
  }

  payload.radiance += payload.throughput * surface.emission * emissiveMisWeight;
  AccumulateEnvironmentDirectLight(payload, surface, sceneInfo, viewDir);
  AccumulateEmissiveDirectLight(payload, surface, sceneInfo, viewDir);

  if(payload.depth >= maxBounces)
  {
    return false;
  }

  RayDesc bounceRay;
  bool    isTransmissionEvent = false;
  if(!BeginContinuationRay(payload, surface, viewDir, bounceRay, isTransmissionEvent))
  {
    return false;
  }

  payload.depth += 1;

  if(enableRussianRoulette && payload.depth > 1)
  {
    const float continueProbability = clamp(SafeMax3(payload.throughput), 0.05, 0.95);
    if(NextRandom(payload.seed) > continueProbability)
    {
      return false;
    }

    payload.throughput /= continueProbability;
  }

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
  return true;
}

#endif
