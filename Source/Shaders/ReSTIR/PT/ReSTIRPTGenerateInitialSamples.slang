#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "nvshaders/sky_functions.h.slang"
#include "ShaderIncludes/Pbr.h.slang"
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PT/Globals.h.slang"

#define PATH_TRACING_CUSTOM_GLOBALS
#define PATH_TRACING_PAYLOAD_EXTRA_FIELDS \
  uint pixelIndex; \
  ReSTIRPTReservoir primaryReservoir; \
  float3 firstBounceHitPosition; \
  float firstBounceHitDistance; \
  float3 firstBounceHitNormal; \
  uint firstBounceHitValid; \
  uint primaryHasContinuation; \
  uint pad0; \
  uint pad1;
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"
#include "ShaderIncludes/ReSTIR/PT/PathReplay.h.slang"

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID    = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo   = pushConst.sceneInfoAddress[0];
  const float2        pixelCenter = float2(launchID) + 0.5;
  const uint          pixelIndex  = FlattenPixelIndex(launchID, uint2(sceneInfo.viewportSize));

  // The primary surface buffer is always written by the initial pass, even if
  // the ray misses, so later passes can distinguish valid and invalid receivers.
  currentSurfaceBuffer[pixelIndex] = EmptyPrimarySurface();

  RayDesc ray;
  ray.Origin    = sceneInfo.cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  PathPayload payload;
  payload.radiance                      = float3(0.0);
  payload.throughput                    = float3(1.0);
  payload.mediumAttenuation             = float3(0.0);
  payload.mediumThickness               = 0.0;
  payload.lastSurfacePosition           = sceneInfo.cameraPosition;
  payload.lastSurfaceNormal             = float3(0.0, 1.0, 0.0);
  payload.lastDirectLightBsdfPdf        = 0.0;
  payload.seed                          = xxhash32(uint3(launchID, pushConst.rngFrameNumber));
  payload.depth                         = 0;
  payload.mediumActive                  = 0;
  payload.lastEnvironmentNeeActive      = 0;
  payload.hasLastBsdfSample             = 0;
  payload.pixelIndex                    = pixelIndex;
  payload.primaryReservoir              = EmptyReservoir();
  payload.firstBounceHitPosition        = float3(0.0);
  payload.firstBounceHitDistance        = 0.0;
  payload.firstBounceHitNormal          = float3(0.0, 1.0, 0.0);
  payload.firstBounceHitValid           = 0u;
  payload.primaryHasContinuation        = 0u;
  payload.pad0                          = 0u;
  payload.pad1                          = 0u;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, payload);

  ReSTIRPTReservoir reservoir = payload.primaryReservoir;

  // The initial pass writes either the transient initial buffer or the history
  // buffer directly, depending on whether later reuse passes are enabled.
  StoreInitialOutput(pixelIndex, reservoir);
  WriteDebugPixel(pixelIndex, reservoir, 1.0, reservoir.valid != 0u ? 1.0 : 0.0, uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone),
                  uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone));
}

[shader("miss")]
void rmissMain(inout PathPayload payload)
{
  AccumulateReSTIRPTMissRadiance(payload);
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
  if(payload.depth > 0)
  {
    ContinueReSTIRPTPath(payload, attr);
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const SurfaceData   surface   = LoadSurfaceData(attr);
  const float3        viewDir   = -WorldRayDirection();
  const GltfInstance  instance  = sceneInfo.instances[InstanceIndex()];
  const uint          initialSampleCount = max(pushConst.initialSampling.numInitialSamples, 1u);

  const float3 baseRadiance = ComputePrimaryBaseRadiance(surface, viewDir, payload.seed);
  const ReSTIRPTPrimarySurface primarySurface = PackPrimarySurface(surface, instance.materialIndex, RayTCurrent(), baseRadiance);
  currentSurfaceBuffer[payload.pixelIndex] = primarySurface;

  if(GetReSTIRPTPathMaxBounceDepth() == 0u)
  {
    return;
  }

  ReSTIRPTReservoir combinedReservoir = EmptyReservoir();
  uint              combineSeed       = payload.seed;
  // Initial sampling may generate more than one first-bounce candidate per
  // receiver and reservoir-sample them immediately.
  for(uint sampleIndex = 0; sampleIndex < initialSampleCount; ++sampleIndex)
  {
    uint candidateSeed = HashUint(payload.seed + 0x9e3779b9u * (sampleIndex + 1u));

    float3 bounceDir;
    float3 bsdfOverPdf         = float3(0.0);
    float  directLightBsdfPdf  = 0.0;
    bool   isTransmissionEvent = false;
    uint   sampledLobeKind     = 0u;
    if(!SampleSurfaceBsdf(surface, viewDir, candidateSeed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
    {
      continue;
    }

    if(abs(dot(surface.geometricNormal, bounceDir)) <= 1.0e-6)
    {
      continue;
    }

    const uint continuationSeed = candidateSeed;

    PathPayload suffixPayload;
    suffixPayload.radiance                 = float3(0.0);
    suffixPayload.throughput               = float3(1.0);
    suffixPayload.mediumAttenuation        = float3(0.0);
    suffixPayload.mediumThickness          = 0.0;
    suffixPayload.lastSurfacePosition      = surface.worldPosition;
    suffixPayload.lastSurfaceNormal        = surface.shadingNormal;
    suffixPayload.lastDirectLightBsdfPdf   = directLightBsdfPdf;
    suffixPayload.seed                     = continuationSeed;
    suffixPayload.depth                    = 1;
    suffixPayload.mediumActive             = 0;
    suffixPayload.lastEnvironmentNeeActive = 0;
    suffixPayload.hasLastBsdfSample        = 1u;
    suffixPayload.pixelIndex               = payload.pixelIndex;
    suffixPayload.primaryReservoir         = EmptyReservoir();
    suffixPayload.firstBounceHitPosition   = float3(0.0);
    suffixPayload.firstBounceHitDistance   = 0.0;
    suffixPayload.firstBounceHitNormal     = float3(0.0, 1.0, 0.0);
    suffixPayload.firstBounceHitValid      = 0u;
    suffixPayload.primaryHasContinuation   = 0u;
    suffixPayload.pad0                     = 0u;
    suffixPayload.pad1                     = 0u;

    if(isTransmissionEvent)
    {
      if(surface.isFrontFace != 0 && HasVolumeAttenuation(surface))
      {
        suffixPayload.mediumAttenuation = BuildAttenuationCoefficient(surface.attenuationColor, surface.attenuationDistance);
        suffixPayload.mediumThickness   = surface.volumeThickness;
        suffixPayload.mediumActive      = 1;
      }
      else if(surface.isFrontFace == 0)
      {
        suffixPayload.mediumAttenuation = float3(0.0);
        suffixPayload.mediumThickness   = 0.0;
        suffixPayload.mediumActive      = 0;
      }
    }

    if(isTransmissionEvent)
    {
      suffixPayload.lastEnvironmentNeeActive = (CanSampleEnvironmentTransmissionExitLight(surface) && directLightBsdfPdf > 0.0) ? 1u : 0u;
    }
    else
    {
      suffixPayload.lastEnvironmentNeeActive = (CanSampleEnvironmentReflectionDirectLight(surface) && directLightBsdfPdf > 0.0) ? 1u : 0u;
    }

    const float3 originNormal = SelectOffsetNormal(surface.geometricNormal, bounceDir);

    RayDesc bounceRay;
    bounceRay.Origin    = OffsetRay(surface.worldPosition, originNormal);
    bounceRay.Direction = bounceDir;
    bounceRay.TMin      = 0.001;
    bounceRay.TMax      = kRayTMax;

    TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, suffixPayload);

    const float3 localOutgoingDirection = normalize(ToLocal(surface.shadingNormal, surface.tangent, surface.bitangent, bounceDir));
    // The candidate stores the replayable first-bounce decision plus the
    // suffix radiance behind it.
    ReSTIRPTReservoir candidate = MakeReservoir(localOutgoingDirection, suffixPayload.radiance, continuationSeed, sampledLobeKind,
                                                suffixPayload.firstBounceHitPosition, suffixPayload.firstBounceHitNormal,
                                                suffixPayload.firstBounceHitValid != 0u, 1.0);
    candidate.targetPdf = EvaluateReservoirCandidateTarget(primarySurface, candidate, sceneInfo.cameraPosition);
    candidate.weight    = candidate.targetPdf > 0.0 ? 1.0 : 0.0;
    candidate.valid     = candidate.targetPdf > 0.0 ? 1u : 0u;
    if(candidate.valid == 0u)
    {
      continue;
    }

    CombineReservoir(combinedReservoir, candidate, candidate.weight * max(candidate.M, 1.0), max(candidate.M, 1.0),
                     RandomFloat(combineSeed), false);
  }

  payload.primaryReservoir       = combinedReservoir;
  payload.primaryHasContinuation = combinedReservoir.valid;
}
