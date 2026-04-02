/*
 * Copyright (c) 2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "nvshaders/sky_functions.h.slang"
#include "ShaderIncludes/Pbr.h.slang"
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIo.h"
#include "NRD.hlsli"
#include "ShaderIncludes/PathTracing/Globals.h.slang"

#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"

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

float3 ComputeDiffuseDemodulationFactor(PathPayload payload)
{
  if(payload.hasPrimarySurface == 0u)
  {
    return float3(1.0, 1.0, 1.0);
  }

  const float3 diffuseFactor = payload.primaryBaseColor * (1.0 - payload.primaryMetalness);
  return max(diffuseFactor, kMinDiffuseDemodulationFactor);
}

void WriteDenoiserGuideBuffers(uint2 pixelPosition, GltfSceneInfo sceneInfo, PathPayload payload)
{
  if(payload.hasPrimarySurface != 0u)
  {
    const float viewZ = mul(float4(payload.primaryWorldPosition, 1.0), sceneInfo.viewMatrix).z;
    const float3 motion = ComputeDenoiserMotionVector(float2(pixelPosition) + 0.5, payload.primaryWorldPosition, sceneInfo);
    motionVectorsImage[int2(pixelPosition)]   = float4(motion, 0.0);
    normalRoughnessImage[int2(pixelPosition)] = PackNrdNormalRoughness(payload.primaryShadingNormal, payload.primaryRoughness);
    baseColorMetalnessImage[int2(pixelPosition)] = PackBaseColorMetalness(payload.primaryBaseColor, payload.primaryMetalness);
    viewZImage[int2(pixelPosition)]           = viewZ;
    return;
  }

  motionVectorsImage[int2(pixelPosition)]   = float4(0.0, 0.0, 0.0, 0.0);
  normalRoughnessImage[int2(pixelPosition)] = float4(0.0, 0.0, 0.0, 0.0);
  baseColorMetalnessImage[int2(pixelPosition)] = float4(0.0, 0.0, 0.0, 0.0);
  viewZImage[int2(pixelPosition)]           = kInvalidDenoiserViewZ;
}

void WriteDenoiserNoisySignals(uint2 pixelPosition, PathPayload payload)
{
  const int2 imagePosition = int2(pixelPosition);

  if(payload.hasPrimarySurface == 0u)
  {
    diffuseRadianceHitDistanceImage[imagePosition]  = float4(0.0, 0.0, 0.0, 0.0);
    specularRadianceHitDistanceImage[imagePosition] = float4(0.0, 0.0, 0.0, 0.0);
    return;
  }

  const GltfSceneInfo sceneInfo         = pushConst.sceneInfoAddress[0];
  const float         primaryViewZ      = mul(float4(payload.primaryWorldPosition, 1.0), sceneInfo.viewMatrix).z;
  const float         diffuseHitDistance  = payload.hasDiffuseDenoiserHitDistance != 0u ? payload.diffuseDenoiserHitDistance : 0.0;
  const float specularHitDistance = payload.hasSpecularDenoiserHitDistance != 0u ? payload.specularDenoiserHitDistance : 0.0;
  const float3 diffuseDemodulationFactor = ComputeDiffuseDemodulationFactor(payload);

  diffuseRadianceHitDistanceImage[imagePosition] =
      PackReblurRadianceHitDistance(max(payload.diffuseDenoiserRadiance, float3(0.0)) / diffuseDemodulationFactor, diffuseHitDistance,
                                    primaryViewZ,
                                    payload.primaryRoughness, true);
  specularRadianceHitDistanceImage[imagePosition] =
      PackReblurRadianceHitDistance(max(payload.specularDenoiserRadiance, float3(0.0)), specularHitDistance, primaryViewZ,
                                    payload.primaryRoughness, false);
}


[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID    = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo   = pushConst.sceneInfoAddress[0];
  const float2        pixelCenter = float2(launchID) + 0.5;

  RayDesc ray;
  ray.Origin    = sceneInfo.cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  PathPayload payload;
  payload.radiance                 = float3(0.0);
  payload.throughput               = float3(1.0);
  payload.mediumAttenuation        = float3(0.0);
  payload.mediumThickness          = 0.0;
  payload.lastSurfacePosition      = sceneInfo.cameraPosition;
  payload.lastSurfaceNormal        = float3(0.0, 1.0, 0.0);
  payload.lastDirectLightBsdfPdf   = 0.0;
  payload.seed                     = xxhash32(uint3(launchID, pushConst.rngFrameNumber));
  payload.depth                    = 0;
  payload.mediumActive             = 0;
  payload.lastEnvironmentNeeActive = 0;
  payload.hasLastBsdfSample        = 0;
  payload.primaryWorldPosition     = float3(0.0);
  payload.primaryRoughness         = 0.0;
  payload.primaryShadingNormal     = float3(0.0);
  payload.primaryBaseColor         = float3(0.0);
  payload.primaryMetalness         = 0.0;
  payload.hasPrimarySurface        = 0;
  payload.diffuseDenoiserRadiance       = float3(0.0);
  payload.diffuseDenoiserHitDistance    = 0.0;
  payload.specularDenoiserRadiance      = float3(0.0);
  payload.specularDenoiserHitDistance   = 0.0;
  payload.hasDiffuseDenoiserHitDistance = 0u;
  payload.hasSpecularDenoiserHitDistance = 0u;
  payload.firstBounceIsSpecular          = 0u;
  payload.awaitingFirstBounceHitDistance = 0u;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, payload);
  WriteDenoiserGuideBuffers(launchID, sceneInfo, payload);
  WriteDenoiserNoisySignals(launchID, payload);

  const float3 sampleRadiance   = max(payload.radiance, float3(0.0));
  float3       resolvedRadiance = sampleRadiance;
  if(IsFinalRadianceAccumulationEnabled() && pushConst.accumulatedFrames > 0)
  {
    const float3 previousAverage = accumulationImage[int2(launchID)].xyz;
    const float  historyWeight   = float(pushConst.accumulatedFrames);
    resolvedRadiance = (previousAverage * historyWeight + sampleRadiance) / (historyWeight + 1.0);
  }

  accumulationImage[int2(launchID)] = float4(resolvedRadiance, 1.0);
  outImage[int2(launchID)]          = float4(resolvedRadiance, 1.0);
}

[shader("miss")]
void rmissMain(inout PathPayload payload)
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

  const float3 contribution = envThroughput * envColor * misWeight;
  payload.radiance += contribution;
  AccumulateDenoiserRadiance(payload, contribution);
  StoreFirstBounceHitDistance(payload, kDenoiserMissHitDistance);
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
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const SurfaceData   surface   = LoadSurfaceData(attr);
  const float3        viewDir   = -WorldRayDirection();

  StorePrimarySurfaceForDenoiser(payload, surface);
  StoreFirstBounceHitDistance(payload, RayTCurrent());

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

  const float3 emissiveContribution = payload.throughput * surface.emission * emissiveMisWeight;
  payload.radiance += emissiveContribution;
  AccumulateDenoiserRadiance(payload, emissiveContribution);
  AccumulateEnvironmentDirectLight(payload, surface, sceneInfo, viewDir);
  AccumulateEmissiveDirectLight(payload, surface, sceneInfo, viewDir);

  if(payload.depth >= pushConst.maxBounces)
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

  if(payload.depth == 0u)
  {
    BeginFirstBounceDenoiserSignal(payload, sampledLobeKind);
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
