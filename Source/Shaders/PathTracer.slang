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
#include "ShaderIncludes/PathTracing/Globals.h.slang"

#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"

[shader("raygeneration")]
void rgenMain()
{
  const uint2 launchID = DispatchRaysIndex().xy;

  const GltfSceneInfo sceneInfo   = pushConst.sceneInfoAddress[0];
  const float2        pixelCenter = float2(launchID) + 0.5;

  RayDesc ray;
  ray.Origin    = sceneInfo.cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  PathPayload payload;
  payload.radiance          = float3(0.0);
  payload.throughput        = float3(1.0);
  payload.mediumAttenuation = float3(0.0);
  payload.mediumThickness   = 0.0;
  payload.lastSurfacePosition = sceneInfo.cameraPosition;
  payload.lastSurfaceNormal = float3(0.0, 1.0, 0.0);
  payload.lastBsdfPdf       = 0.0;
  payload.seed              = xxhash32(uint3(launchID, pushConst.rngFrameNumber));
  payload.depth             = 0;
  payload.mediumActive      = 0;
  payload.lastEnvironmentNeeActive = 0;
  payload.hasLastBsdfSample = 0;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, payload);

  const float3 sampleRadiance   = max(payload.radiance, float3(0.0));
  float3       resolvedRadiance = sampleRadiance;

  if(IsAccumulationEnabled() && pushConst.accumulatedFrames > 0)
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
    // Miss-side MIS only competes against the scene-wide explicit environment sampler. When a surface lets the
    // BSDF continuation ray own environment transport, lastEnvironmentNeeActive is zero and this branch is skipped.
    const float lightPdf = EvaluateEnvironmentLightPdf(sceneInfo, payload.lastSurfaceNormal, WorldRayDirection());
    if(lightPdf > 0.0 && payload.lastBsdfPdf > 0.0)
    {
      misWeight = MisMixWeight(payload.lastBsdfPdf, lightPdf);
    }
  }

  float3 envThroughput = payload.throughput;
  if(payload.mediumActive != 0)
  {
    envThroughput *= EvaluateMediumTransmittance(payload.mediumAttenuation, payload.mediumThickness);
  }

  payload.radiance += envThroughput * envColor * misWeight;
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

  ApplyCurrentMediumAttenuation(payload, RayTCurrent());

  float emissiveMisWeight = 1.0;
  if(payload.hasLastBsdfSample != 0 && SafeMax3(surface.emission) > 0.0)
  {
    const float lightPdf =
        EvaluateCurrentEmissiveHitPdf(sceneInfo, InstanceIndex(), PrimitiveIndex(), payload.lastSurfacePosition, surface.worldPosition);
    if(lightPdf > 0.0)
    {
      emissiveMisWeight = MisMixWeight(payload.lastBsdfPdf, lightPdf);
    }
  }

  payload.radiance += payload.throughput * surface.emission * emissiveMisWeight;
  AccumulateEnvironmentDirectLight(payload, surface, sceneInfo, viewDir);
  AccumulateEmissiveDirectLight(payload, surface, sceneInfo, viewDir);

  if(payload.depth >= pushConst.maxBounces)
  {
    return;
  }

  float3 bounceDir;
  float3 bsdfOverPdf;
  float  bsdfPdf = 0.0;
  bool   isTransmissionEvent = false;
  if(!SampleSurfaceBsdf(surface, viewDir, payload.seed, bounceDir, bsdfOverPdf, bsdfPdf, isTransmissionEvent))
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

  // Russian roulette is only there to terminate long, low-energy paths. We
  // start after the first secondary bounce so single-bounce comparisons stay
  // predictable while longer paths remain stable.
  if(payload.depth > 1)
  {
    const float continueProbability = clamp(SafeMax3(nextThroughput), 0.05, 0.95);
    if(NextRandom(payload.seed) > continueProbability)
    {
      return;
    }

    nextThroughput /= continueProbability;
  }

  payload.throughput        = nextThroughput;
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
  payload.lastSurfacePosition = surface.worldPosition;
  payload.lastSurfaceNormal =
      (isTransmissionEvent && CanSampleEnvironmentTransmissionExitLight(surface)) ? -surface.shadingNormal : surface.shadingNormal;
  payload.lastBsdfPdf       = bsdfPdf;

  bool environmentNeeActive = false;
  if(isTransmissionEvent)
  {
    environmentNeeActive = CanSampleEnvironmentTransmissionExitLight(surface);
  }
  else
  {
    environmentNeeActive = CanSampleEnvironmentReflectionDirectLight(surface) && !ShouldBsdfOwnEnvironmentTransport(surface);
  }
  payload.lastEnvironmentNeeActive = environmentNeeActive ? 1u : 0u;
  payload.hasLastBsdfSample = 1;

  const float3 originNormal = SelectOffsetNormal(surface.geometricNormal, bounceDir);

  RayDesc bounceRay;
  bounceRay.Origin    = OffsetRay(surface.worldPosition, originNormal);
  bounceRay.Direction = bounceDir;
  bounceRay.TMin      = 0.001;
  bounceRay.TMax      = kRayTMax;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
}
