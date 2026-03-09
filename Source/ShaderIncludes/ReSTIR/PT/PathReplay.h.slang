#ifndef RESTIR_PT_PATH_REPLAY_H
#define RESTIR_PT_PATH_REPLAY_H

#include "ShaderIncludes/ReSTIR/PT/Reservoir.h.slang"
#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"

void AccumulateReSTIRPTMissRadiance(inout PathPayload payload)
{
  // Final replay and suffix continuation both terminate here, so this miss
  // shader applies the same MIS/environment logic as the baseline path tracer.
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

float3 ComputePrimaryBaseRadiance(SurfaceData surface, float3 viewDir, inout uint seed)
{
  // The primary surface stores direct/base lighting separately so final replay
  // only reconstructs the continuation behind the selected candidate.
  PathPayload lightingPayload;
  lightingPayload = (PathPayload)0;
  lightingPayload.radiance                 = surface.emission;
  lightingPayload.throughput               = float3(1.0);
  lightingPayload.mediumAttenuation        = float3(0.0);
  lightingPayload.mediumThickness          = 0.0;
  lightingPayload.lastSurfacePosition      = surface.worldPosition;
  lightingPayload.lastSurfaceNormal        = surface.shadingNormal;
  lightingPayload.lastDirectLightBsdfPdf   = 0.0;
  lightingPayload.seed                     = seed;
  lightingPayload.depth                    = 0;
  lightingPayload.mediumActive             = 0;
  lightingPayload.lastEnvironmentNeeActive = 0;
  lightingPayload.hasLastBsdfSample        = 0;

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  AccumulateEnvironmentDirectLight(lightingPayload, surface, sceneInfo, viewDir);
  AccumulateEmissiveDirectLight(lightingPayload, surface, sceneInfo, viewDir);
  seed = lightingPayload.seed;

  return lightingPayload.radiance;
}

bool InitializeReplayPayload(out PathPayload payload, ReSTIRPTPrimarySurface primarySurface, ReSTIRPTReservoir reservoir, out float3 worldDirection)
{
  // Reconstruct the exact continuation state needed to replay the chosen
  // candidate after reservoir resampling has finished.
  payload                          = (PathPayload)0;
  payload.radiance                 = primarySurface.baseRadiance;
  payload.throughput               = float3(1.0);
  payload.mediumAttenuation        = float3(0.0);
  payload.mediumThickness          = 0.0;
  payload.lastSurfacePosition      = primarySurface.worldPosition;
  payload.lastSurfaceNormal        = primarySurface.shadingNormal;
  payload.lastDirectLightBsdfPdf   = 0.0;
  payload.seed                     = reservoir.continuationSeed;
  payload.depth                    = 1;
  payload.mediumActive             = 0;
  payload.lastEnvironmentNeeActive = 0;
  payload.hasLastBsdfSample        = 0;

  worldDirection = GetReservoirWorldOutgoingDirection(primarySurface, reservoir);
  if(dot(worldDirection, worldDirection) <= 0.0)
  {
    return false;
  }

  const SurfaceData surfaceData = UnpackPrimarySurface(primarySurface);
  const float3      viewDir     = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surfaceData.worldPosition);

  float  samplePdf           = 0.0;
  float  directLightPdf      = 0.0;
  bool   isTransmissionEvent = false;
  const float3 bsdf = EvaluateReservoirCandidateBsdfContribution(surfaceData, viewDir, worldDirection, reservoir.candidateKind, samplePdf,
                                                                 directLightPdf, isTransmissionEvent);
  if(samplePdf <= 0.0 || SafeMax3(bsdf) <= 0.0)
  {
    return false;
  }

  payload.throughput = max(bsdf / samplePdf, float3(0.0));
  if(SafeMax3(payload.throughput) <= 0.0)
  {
    return false;
  }

  if(isTransmissionEvent)
  {
    if(surfaceData.isFrontFace != 0 && HasVolumeAttenuation(surfaceData))
    {
      payload.mediumAttenuation = BuildAttenuationCoefficient(surfaceData.attenuationColor, surfaceData.attenuationDistance);
      payload.mediumThickness   = surfaceData.volumeThickness;
      payload.mediumActive      = 1;
    }
    else if(surfaceData.isFrontFace == 0)
    {
      payload.mediumAttenuation = float3(0.0);
      payload.mediumThickness   = 0.0;
      payload.mediumActive      = 0;
    }
  }

  payload.lastDirectLightBsdfPdf = directLightPdf;
  if(isTransmissionEvent)
  {
    payload.lastEnvironmentNeeActive = (CanSampleEnvironmentTransmissionExitLight(surfaceData) && directLightPdf > 0.0) ? 1u : 0u;
  }
  else
  {
    payload.lastEnvironmentNeeActive = (CanSampleEnvironmentReflectionDirectLight(surfaceData) && directLightPdf > 0.0) ? 1u : 0u;
  }
  payload.hasLastBsdfSample = 1u;
  return true;
}

void ContinueReSTIRPTPath(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  // This is the shared continuation path used by both initial candidate
  // generation and final replay.
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const SurfaceData   surface   = LoadSurfaceData(attr);
  const float3        viewDir   = -WorldRayDirection();

  if(payload.depth == 1 && payload.firstBounceHitValid == 0u)
  {
    // The first continuation hit becomes the optional reconnection endpoint
    // used by temporal and spatial shifting.
    payload.firstBounceHitPosition = surface.worldPosition;
    payload.firstBounceHitDistance = RayTCurrent();
    payload.firstBounceHitNormal   = surface.geometricNormal;
    payload.firstBounceHitValid    = 1u;
  }

  ApplyCurrentMediumAttenuation(payload, RayTCurrent());

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

  if(payload.depth >= GetReSTIRPTPathMaxBounceDepth())
  {
    return;
  }

  float3 bounceDir;
  float3 bsdfOverPdf         = float3(0.0);
  float  directLightBsdfPdf  = 0.0;
  bool   isTransmissionEvent = false;
  uint   sampledLobeKind     = 0u;
  if(!SampleSurfaceBsdf(surface, viewDir, payload.seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
  {
    return;
  }

  const float geometricDot = dot(surface.geometricNormal, bounceDir);
  if(abs(geometricDot) <= 1.0e-6)
  {
    return;
  }

  float3 nextThroughput = payload.throughput * max(bsdfOverPdf, float3(0.0));
  if(SafeMax3(nextThroughput) <= 0.0)
  {
    return;
  }

  payload.depth += 1;

  if(payload.depth > 1)
  {
    const float continueProbability = clamp(SafeMax3(nextThroughput), 0.05, 0.95);
    if(NextRandom(payload.seed) > continueProbability)
    {
      return;
    }

    nextThroughput /= continueProbability;
  }

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

  const float3 originNormal = SelectOffsetNormal(surface.geometricNormal, bounceDir);

  RayDesc bounceRay;
  bounceRay.Origin    = OffsetRay(surface.worldPosition, originNormal);
  bounceRay.Direction = bounceDir;
  bounceRay.TMin      = 0.001;
  bounceRay.TMax      = kRayTMax;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
}

#endif
