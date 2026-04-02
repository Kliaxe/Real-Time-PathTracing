#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/GI/Globals.h.slang"

#define PATH_TRACING_PAYLOAD_EXTRA_FIELDS \
  uint pixelIndex; \
  ReSTIRGIReservoir primaryReservoir; \
  float secondarySamplePdf; \
  uint secondarySampleValid; \
  uint secondarySampleKind; \
  uint pad0; \
  float3 secondarySamplePosition; \
  float3 secondarySampleNormal; \
  float3 secondarySampleDirection;
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/DirectLighting.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Debug.h.slang"

[shader("raygeneration")]
void rgenMain()
{
  const uint2 launchID    = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const uint pixelIndex   = FlattenPixelIndex(launchID, uint2(sceneInfo.viewportSize));
  const float2 pixelCenter = float2(launchID) + 0.5;

  currentSurfaceBuffer[pixelIndex] = EmptyGIPrimarySurface();

  RayDesc ray;
  ray.Origin    = sceneInfo.cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  PathPayload payload;
  payload                          = (PathPayload)0;
  payload.pixelIndex               = pixelIndex;
  payload.primaryReservoir         = EmptyGIReservoir();
  payload.secondarySamplePdf       = 0.0;
  payload.secondarySampleValid     = 0u;
  payload.secondarySampleKind      = uint(ReSTIRGISampleKind::eReSTIRGISampleKindSurface);
  payload.secondarySamplePosition  = float3(0.0);
  payload.secondarySampleNormal    = float3(0.0, 1.0, 0.0);
  payload.secondarySampleDirection = float3(0.0, 1.0, 0.0);
  payload.seed                     = xxhash32(uint3(launchID, giParams.runtimeParams.frameIndex));

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, payload);

  StoreGIReservoir(payload.primaryReservoir, giParams.reservoirBufferParams,
                   PixelPosToReservoirPos(launchID, giParams.runtimeParams.activeCheckerboardField),
                   giParams.bufferIndices.initialSamplingOutputBufferIndex);
  giInitialSampleBuffer[pixelIndex] = payload.primaryReservoir;
  WriteGIDebugPixel(pixelIndex, currentSurfaceBuffer[pixelIndex], payload.primaryReservoir, 1.0, payload.primaryReservoir.M,
                    uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone), uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone));
}

[shader("miss")]
void rmissMain(inout PathPayload payload)
{
  if(payload.depth == 0u)
  {
    payload.primaryReservoir = EmptyGIReservoir();
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  float3 sampleRadiance         = SampleEnvironment(sceneInfo, WorldRayDirection());
  if(payload.mediumActive != 0u)
  {
    sampleRadiance *= EvaluateMediumTransmittance(payload.mediumAttenuation, payload.mediumThickness);
  }

  payload.radiance                = sampleRadiance;
  payload.secondarySampleValid    = 1u;
  payload.secondarySampleKind     = uint(ReSTIRGISampleKind::eReSTIRGISampleKindEnvironment);
  payload.secondarySamplePosition = float3(0.0);
  payload.secondarySampleNormal   = float3(0.0);
  payload.secondarySampleDirection = normalize(WorldRayDirection());
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
  if(payload.depth > 0u)
  {
    const SurfaceData secondarySurface = LoadSurfaceData(attr);
    const float3 secondaryViewDir      = -WorldRayDirection();

    float3 sampleRadiance = ComputeSurfaceBaseRadiance(secondarySurface, secondaryViewDir, payload.seed);
    if(payload.mediumActive != 0u)
    {
      sampleRadiance *= EvaluateMediumTransmittance(payload.mediumAttenuation, RayTCurrent());
    }

    payload.radiance                = sampleRadiance;
    payload.secondarySamplePosition = secondarySurface.worldPosition;
    payload.secondarySampleNormal   = secondarySurface.geometricNormal;
    payload.secondarySampleDirection = float3(0.0);
    payload.secondarySampleValid    = 1u;
    payload.secondarySampleKind     = uint(ReSTIRGISampleKind::eReSTIRGISampleKindSurface);
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const SurfaceData   primarySurfaceData = LoadSurfaceData(attr);
  const float3        viewDir            = -WorldRayDirection();
  const GltfInstance  instance           = sceneInfo.instances[InstanceIndex()];

  const float3 primaryBaseRadiance = ComputeSurfaceBaseRadiance(primarySurfaceData, viewDir, payload.seed);
  const GISurface primarySurface = PackGIPrimarySurface(primarySurfaceData, instance.materialIndex, RayTCurrent(), primaryBaseRadiance);
  currentSurfaceBuffer[payload.pixelIndex] = primarySurface;

  ReSTIRGIReservoir initialReservoir = EmptyGIReservoir();
  float             selectedTargetPdf = 0.0;

  if(CanSampleEnvironmentReflectionDirectLight(primarySurfaceData))
  {
    float3 environmentDirection = float3(0.0);
    float3 environmentRadiance  = float3(0.0);
    float  environmentPdf       = 0.0;
    if(SampleEnvironmentBaseLight(sceneInfo, primarySurfaceData.shadingNormal, payload.seed, environmentDirection, environmentRadiance,
                                  environmentPdf))
    {
      if(TraceVisibility(primarySurfaceData.worldPosition, primarySurfaceData.geometricNormal, environmentDirection))
      {
        float environmentBsdfPdf = 0.0;
        EvaluateDirectLightBsdf(primarySurfaceData, viewDir, environmentDirection, environmentBsdfPdf);
        const float environmentSourcePdf = environmentPdf + environmentBsdfPdf;
        const ReSTIRGIReservoir environmentReservoir =
            MakeGIEnvironmentReservoir(environmentDirection, environmentRadiance, environmentSourcePdf);
        const float environmentTargetPdf = GIGetSampleTargetPdfForSurface(environmentReservoir, primarySurface);
        if(environmentTargetPdf > 0.0)
        {
          CombineGIReservoirs(initialReservoir, environmentReservoir, 0.5, environmentTargetPdf);
          selectedTargetPdf = environmentTargetPdf;
        }
      }
    }
  }

  float3 bounceDir           = float3(0.0);
  float3 bsdfOverPdf         = float3(0.0);
  float  directLightBsdfPdf  = 0.0;
  bool   isTransmissionEvent = false;
  uint   sampledLobeKind     = 0u;
  if(SampleSurfaceBsdf(primarySurfaceData, viewDir, payload.seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent,
                       sampledLobeKind)
     && sampledLobeKind == 0u
     && abs(dot(primarySurfaceData.geometricNormal, bounceDir)) > 1.0e-6)
  {
    const float samplePdf = directLightBsdfPdf;
    if(samplePdf > 0.0)
    {
      payload.radiance                = float3(0.0);
      payload.secondarySamplePdf      = samplePdf;
      payload.secondarySampleValid    = 0u;
      payload.secondarySampleKind     = uint(ReSTIRGISampleKind::eReSTIRGISampleKindSurface);
      payload.secondarySamplePosition = float3(0.0);
      payload.secondarySampleNormal   = float3(0.0, 1.0, 0.0);
      payload.secondarySampleDirection = float3(0.0, 1.0, 0.0);
      payload.depth                   = 1u;

      if(isTransmissionEvent)
      {
        if(primarySurfaceData.isFrontFace != 0u && HasVolumeAttenuation(primarySurfaceData))
        {
          payload.mediumAttenuation = BuildAttenuationCoefficient(primarySurfaceData.attenuationColor, primarySurfaceData.attenuationDistance);
          payload.mediumThickness   = primarySurfaceData.volumeThickness;
          payload.mediumActive      = 1u;
        }
        else if(primarySurfaceData.isFrontFace == 0u)
        {
          payload.mediumAttenuation = float3(0.0);
          payload.mediumThickness   = 0.0;
          payload.mediumActive      = 0u;
        }
      }
      else
      {
        payload.mediumAttenuation = float3(0.0);
        payload.mediumThickness   = 0.0;
        payload.mediumActive      = 0u;
      }

      RayDesc bounceRay;
      bounceRay.Origin    = OffsetRay(primarySurfaceData.worldPosition, SelectOffsetNormal(primarySurfaceData.geometricNormal, bounceDir));
      bounceRay.Direction = bounceDir;
      bounceRay.TMin      = 0.001;
      bounceRay.TMax      = kRayTMax;

      TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);

      if(payload.secondarySampleValid != 0u)
      {
        ReSTIRGIReservoir secondaryReservoir = EmptyGIReservoir();
        if(payload.secondarySampleKind == uint(ReSTIRGISampleKind::eReSTIRGISampleKindEnvironment))
        {
          const float environmentPdf =
              EvaluateEnvironmentBaseLightPdf(sceneInfo, primarySurfaceData.shadingNormal, payload.secondarySampleDirection);
          const float environmentSourcePdf = payload.secondarySamplePdf + environmentPdf;
          secondaryReservoir = MakeGIEnvironmentReservoir(payload.secondarySampleDirection, payload.radiance, environmentSourcePdf);
        }
        else
        {
          secondaryReservoir = MakeGISurfaceReservoir(payload.secondarySamplePosition, payload.secondarySampleNormal, payload.radiance,
                                                      payload.secondarySamplePdf);
        }

        const float secondaryTargetPdf = GIGetSampleTargetPdfForSurface(secondaryReservoir, primarySurface);
        if(secondaryTargetPdf > 0.0)
        {
          const float random = (IsValidGIReservoir(initialReservoir) ? NextRandom(payload.seed) : 0.5);
          if(CombineGIReservoirs(initialReservoir, secondaryReservoir, random, secondaryTargetPdf))
          {
            selectedTargetPdf = secondaryTargetPdf;
          }
        }
      }
    }
  }

  if(IsValidGIReservoir(initialReservoir) && selectedTargetPdf > 0.0)
  {
    FinalizeGIResampling(initialReservoir, 1.0, selectedTargetPdf * float(initialReservoir.M));
    payload.primaryReservoir = initialReservoir;
  }
  else
  {
    payload.primaryReservoir = EmptyGIReservoir();
  }
}
