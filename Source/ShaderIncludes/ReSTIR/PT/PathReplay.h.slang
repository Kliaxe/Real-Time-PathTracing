#ifndef RESTIR_PT_PATH_REPLAY_H
#define RESTIR_PT_PATH_REPLAY_H

#include "ShaderIncludes/ReSTIR/PT/Reservoir.h.slang"
#include "ShaderIncludes/PathTracing/Continuation.h.slang"

void AccumulateReSTIRPTMissRadiance(inout PathPayload payload)
{
  AccumulateContinuationMissRadiance(payload);
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

  ContinuePathFromSurface(payload, surface, viewDir, GetReSTIRPTPathMaxBounceDepth(), true);
}

#endif
