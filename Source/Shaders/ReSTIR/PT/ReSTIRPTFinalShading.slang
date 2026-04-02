#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "nvshaders/sky_functions.h.slang"
#include "NRD.hlsli"
#include "ShaderIncludes/Pbr.h.slang"
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PT/Globals.h.slang"

#define PATH_TRACING_CUSTOM_GLOBALS
#define PATH_TRACING_PAYLOAD_EXTRA_FIELDS \
  float3 firstBounceHitPosition; \
  float firstBounceHitDistance; \
  float3 firstBounceHitNormal; \
  uint firstBounceHitValid;
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"
#include "ShaderIncludes/ReSTIR/PT/PathReplay.h.slang"
#include "ShaderIncludes/ReSTIR/PT/Debug.h.slang"

static const float kInvalidDenoiserViewZ     = 1.0e32;
static const float kDenoiserMissHitDistance  = 65504.0;
static const float4 kReblurHitDistanceParameters = float4(3.0, 0.1, 20.0, -25.0);
static const float3 kMinDiffuseDemodulationFactor = float3(0.05, 0.05, 0.05);

float4 PackNrdNormalRoughness(float3 shadingNormal, float roughness)
{
  return NRD_FrontEnd_PackNormalAndRoughness(shadingNormal, roughness, 0.0);
}

float4 PackReblurRadianceHitDistance(float3 radiance, float hitDistance, float viewZ, float roughness, bool isDiffuse)
{
  const float normalizedHitDistance =
      REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, kReblurHitDistanceParameters, isDiffuse ? 1.0 : roughness);
  return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normalizedHitDistance, true);
}

float4 PackBaseColorMetalness(float3 baseColor, float metalness)
{
  return float4(saturate(baseColor), saturate(metalness));
}

float3 ComputeDiffuseDemodulationFactor(ReSTIRPTPrimarySurface primarySurface)
{
  const float3 diffuseFactor = primarySurface.albedo * (1.0 - primarySurface.metallic);
  return max(diffuseFactor, kMinDiffuseDemodulationFactor);
}

void WriteInvalidDenoiserSignals(int2 pixelPosition)
{
  motionVectorsImage[pixelPosition]               = float4(0.0, 0.0, 0.0, 0.0);
  normalRoughnessImage[pixelPosition]             = float4(0.0, 0.0, 0.0, 0.0);
  baseColorMetalnessImage[pixelPosition]          = float4(0.0, 0.0, 0.0, 0.0);
  viewZImage[pixelPosition]                       = kInvalidDenoiserViewZ;
  diffuseRadianceHitDistanceImage[pixelPosition]  = float4(0.0, 0.0, 0.0, 0.0);
  specularRadianceHitDistanceImage[pixelPosition] = float4(0.0, 0.0, 0.0, 0.0);
}

void WriteDenoiserSignals(int2 pixelPosition,
                          GltfSceneInfo sceneInfo,
                          ReSTIRPTPrimarySurface primarySurface,
                          ReSTIRPTReservoir reservoir,
                          float3 resolvedContinuation)
{
  if(!IsValidPrimarySurface(primarySurface))
  {
    WriteInvalidDenoiserSignals(pixelPosition);
    return;
  }

  const float viewZ = mul(float4(primarySurface.worldPosition, 1.0), sceneInfo.viewMatrix).z;
  const float3 motion = ComputeDenoiserMotionVector(float2(pixelPosition) + 0.5, primarySurface.worldPosition, sceneInfo);
  motionVectorsImage[pixelPosition]   = float4(motion, 0.0);
  normalRoughnessImage[pixelPosition] = PackNrdNormalRoughness(primarySurface.shadingNormal, primarySurface.roughness);
  baseColorMetalnessImage[pixelPosition] = PackBaseColorMetalness(primarySurface.albedo, primarySurface.metallic);
  viewZImage[pixelPosition]           = viewZ;

  float3 diffuseRadiance  = max(primarySurface.baseRadiance, float3(0.0));
  float3 specularRadiance = float3(0.0);
  float  diffuseHitDistance = 0.0;
  float  specularHitDistance = 0.0;
  const float3 diffuseDemodulationFactor = ComputeDiffuseDemodulationFactor(primarySurface);

  if(IsValidReservoir(reservoir) && SafeMax3(resolvedContinuation) > 0.0)
  {
    const bool firstBounceIsSpecular = IsSpecularDenoiserLobe(reservoir.candidateKind);
    const float hitDistance = (reservoir.reconnectionValid != 0u)
                                  ? length(reservoir.reconnectionPoint - primarySurface.worldPosition)
                                  : kDenoiserMissHitDistance;

    if(firstBounceIsSpecular)
    {
      specularRadiance = resolvedContinuation;
      specularHitDistance = hitDistance;
    }
    else
    {
      diffuseRadiance += resolvedContinuation;
      diffuseHitDistance = hitDistance;
    }
  }

  diffuseRadianceHitDistanceImage[pixelPosition] =
      PackReblurRadianceHitDistance(diffuseRadiance / diffuseDemodulationFactor, diffuseHitDistance, viewZ, primarySurface.roughness,
                                    true);
  specularRadianceHitDistanceImage[pixelPosition] =
      PackReblurRadianceHitDistance(specularRadiance, specularHitDistance, viewZ, primarySurface.roughness, false);
}

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID    = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo   = pushConst.sceneInfoAddress[0];
  const uint          pixelIndex  = FlattenPixelIndex(launchID, uint2(sceneInfo.viewportSize));
  const float2        pixelCenter = float2(launchID) + 0.5;

  float3 resolvedRadiance = float3(0.0);
  float3 resolvedContinuation = float3(0.0);

  const ReSTIRPTPrimarySurface primarySurface = currentSurfaceBuffer[pixelIndex];
  if(IsValidPrimarySurface(primarySurface))
  {
    // Base radiance already contains the primary-hit direct lighting. Final
    // replay only reconstructs the selected continuation.
    resolvedRadiance = primarySurface.baseRadiance;

    const ReSTIRPTReservoir reservoir = currentHistoryReservoirBuffer[pixelIndex];
    if(IsValidReservoir(reservoir))
    {
      float3 worldDirection = float3(0.0);
      PathPayload payload;
      if(InitializeReplayPayload(payload, primarySurface, reservoir, worldDirection))
      {
        const float3 originNormal = SelectOffsetNormal(primarySurface.geometricNormal, worldDirection);

        RayDesc bounceRay;
        bounceRay.Origin    = OffsetRay(primarySurface.worldPosition, originNormal);
        bounceRay.Direction = worldDirection;
        bounceRay.TMin      = 0.001;
        bounceRay.TMax      = kRayTMax;

        TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
        // The replay payload starts from base radiance, so only the suffix is
        // scaled by the final reservoir weight here.
        const float3 replayedContinuation = max(payload.radiance - primarySurface.baseRadiance, float3(0.0));
        resolvedContinuation = replayedContinuation * reservoir.weight;
        resolvedRadiance = primarySurface.baseRadiance + resolvedContinuation;
      }
    }
  }
  else
  {
    const float3 primaryDirection = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
    resolvedRadiance              = SampleEnvironment(sceneInfo, primaryDirection);
  }

  WriteDenoiserSignals(int2(launchID), sceneInfo, primarySurface,
                       IsValidPrimarySurface(primarySurface) ? currentHistoryReservoirBuffer[pixelIndex] : EmptyReservoir(), resolvedContinuation);

  resolvedRadiance = SanitizeRadiance(resolvedRadiance);
  resolvedRadiance = VisualizeReSTIRPTDebug(pixelIndex, resolvedRadiance);
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDisabled) && IsReSTIRPTAccumulationEnabled()
     && pushConst.accumulatedFrames > 0)
  {
    const float3 previousAverage = accumulationImage[int2(launchID)].xyz;
    const float  historyWeight   = float(pushConst.accumulatedFrames);
    resolvedRadiance = (previousAverage * historyWeight + resolvedRadiance) / (historyWeight + 1.0);
  }

  accumulationImage[int2(launchID)] = float4(resolvedRadiance, 1.0);
  outImage[int2(launchID)]          = float4(resolvedRadiance, 1.0);
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
  ContinueReSTIRPTPath(payload, attr);
}
