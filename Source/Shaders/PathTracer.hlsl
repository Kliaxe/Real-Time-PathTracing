// Reference path tracer
// Plain unidirectional path tracer with next event estimation, sharing its path tracing code with ReSTIR PT initial sampling.
// Radiance is summed directly into the payload, so this is the estimator the resampled renderer is validated against. It also writes NRD's guide buffers and demodulated radiance signals when denoising is on.

#include <Common/ShaderTypes.h>
#include "ShaderIncludes/Random.hlsli"
#include "ShaderIncludes/Sky.hlsli"
#include "ShaderIncludes/Camera.hlsli"
#include "ShaderIo.h"
#include "NRD.hlsli"
#include "ShaderIncludes/PathTracing/Globals.hlsli"

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"
#include "ShaderIncludes/PathTracing/Volume.hlsli"
#include "ShaderIncludes/PathTracing/Hdri.hlsli"
#include "ShaderIncludes/PathTracing/Disney.hlsli"
#include "ShaderIncludes/PathTracing/Intersection.hlsli"
#include "ShaderIncludes/PathTracing/Lights.hlsli"

// Denoiser constants
// kInvalidDenoiserViewZ marks a pixel with no primary hit. kDenoiserMissHitDistance is reported for a first bounce that escapes the scene; 65504 is the largest finite 16-bit float.
// The demodulation factors are floored so dividing radiance by them cannot blow up on black or non-reflective materials.

static const float  kInvalidDenoiserViewZ          = 1.0e32;
static const float  kDenoiserMissHitDistance       = 65504.0;
static const float3 kMinDiffuseDemodulationFactor  = float3(0.05, 0.05, 0.05);
static const float3 kMinSpecularDemodulationFactor = float3(0.02, 0.02, 0.02);

float4 PackNrdNormalRoughness(float3 shadingNormal, float roughness)
{
  return NRD_FrontEnd_PackNormalAndRoughness(shadingNormal, roughness, 0.0);
}

// Diffuse signals normalize their hit distance as if fully rough; specular ones use the surface roughness.
float4 PackReblurRadianceHitDistance(float3 radiance, float hitDistance, float viewZ, float roughness, bool isDiffuse)
{
  const float normalizedHitDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, pushConst.reblurHitDistanceParams, isDiffuse ? 1.0 : roughness);

  return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normalizedHitDistance, true);
}

float4 PackBaseColorMetalness(float3 baseColor, float metalness)
{
  return float4(saturate(baseColor), saturate(metalness));
}

// Material response to divide out of each radiance signal before REBLUR. A pixel with no primary hit gets unit factors, which leave its (zero) radiance unchanged.
void ComputeDenoiserMaterialFactors(PathPayload payload, out float3 diffuseFactor, out float3 specularFactor)
{
  if(payload.hasPrimarySurface == 0u)
  {
    diffuseFactor  = float3(1.0, 1.0, 1.0);
    specularFactor = float3(1.0, 1.0, 1.0);

    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();
  const float3        viewDir   = normalize(sceneInfo.cameraPosition - payload.primaryWorldPosition);
  const float3        rf0       = lerp(float3(0.04, 0.04, 0.04), payload.primaryBaseColor, payload.primaryMetalness);

  // Demodulation factors
  // NRD recommends filtering material-independent radiance. The preintegrated specular factor comes from NRD_MaterialFactors and is stored for NrdCompose to replay verbatim.
  // The diffuse factor must be the one NrdCompose recomputes from base colour and metalness, not NRD_MaterialFactors' diffuse output: compose multiplies back by its own formula, so dividing by a different quantity would scale the denoised diffuse signal. ReSTIRPTFinalShading uses the same pair for the same reason.

  float3 unusedDiffuseFactor;

  NRD_MaterialFactors(payload.primaryShadingNormal, viewDir, payload.primaryBaseColor, rf0, payload.primaryRoughness, unusedDiffuseFactor, specularFactor);

  diffuseFactor  = max(saturate(payload.primaryBaseColor) * (1.0 - saturate(payload.primaryMetalness)), kMinDiffuseDemodulationFactor);
  specularFactor = max(specularFactor, kMinSpecularDemodulationFactor);
}

// Writes the per-pixel geometry NRD reprojects and filters with. A pixel with no primary hit still gets deterministic values, so NRD never reads an uninitialized texel.
void WriteDenoiserGuideBuffers(uint2 pixelPosition, GltfSceneInfo sceneInfo, PathPayload payload)
{
  if(payload.hasPrimarySurface != 0u)
  {
    const float  viewZ  = mul(float4(payload.primaryWorldPosition, 1.0), sceneInfo.viewMatrix).z;
    const float3 motion = ComputeDenoiserMotionVector((float2)pixelPosition + 0.5, payload.primaryWorldPosition, sceneInfo);

    motionVectorsImage[(int2)pixelPosition]      = float4(motion, 0.0);
    normalRoughnessImage[(int2)pixelPosition]    = PackNrdNormalRoughness(payload.primaryShadingNormal, payload.primaryRoughness);
    baseColorMetalnessImage[(int2)pixelPosition] = PackBaseColorMetalness(payload.primaryBaseColor, payload.primaryMetalness);
    viewZImage[(int2)pixelPosition]              = viewZ;

    return;
  }

  motionVectorsImage[(int2)pixelPosition]              = float4(0.0, 0.0, 0.0, 0.0);
  normalRoughnessImage[(int2)pixelPosition]            = float4(0.0, 0.0, 0.0, 0.0);
  baseColorMetalnessImage[(int2)pixelPosition]         = float4(0.0, 0.0, 0.0, 0.0);
  viewZImage[(int2)pixelPosition]                      = kInvalidDenoiserViewZ;
  specularDemodulationFactorImage[(int2)pixelPosition] = float4(1.0, 1.0, 1.0, 1.0);
}

// Writes the demodulated diffuse and specular radiance with their first-bounce hit distances. A lobe with no recorded hit distance is written with zero, which NRD treats as "not sampled here".
void WriteDenoiserNoisySignals(uint2 pixelPosition, PathPayload payload)
{
  const int2 imagePosition = (int2)pixelPosition;

  if(payload.hasPrimarySurface == 0u)
  {
    diffuseRadianceHitDistanceImage[imagePosition]  = float4(0.0, 0.0, 0.0, 0.0);
    specularRadianceHitDistanceImage[imagePosition] = float4(0.0, 0.0, 0.0, 0.0);
    specularDemodulationFactorImage[imagePosition]  = float4(1.0, 1.0, 1.0, 1.0);

    return;
  }

  const GltfSceneInfo sceneInfo           = pushConst.sceneInfoAddress.Get();
  const float         primaryViewZ        = mul(float4(payload.primaryWorldPosition, 1.0), sceneInfo.viewMatrix).z;
  const float         diffuseHitDistance  = payload.hasDiffuseDenoiserHitDistance != 0u ? payload.diffuseDenoiserHitDistance : 0.0;
  const float         specularHitDistance = payload.hasSpecularDenoiserHitDistance != 0u ? payload.specularDenoiserHitDistance : 0.0;

  float3 diffuseDemodulationFactor;
  float3 specularDemodulationFactor;

  ComputeDenoiserMaterialFactors(payload, diffuseDemodulationFactor, specularDemodulationFactor);

  specularDemodulationFactorImage[imagePosition] = float4(specularDemodulationFactor, 1.0);

  diffuseRadianceHitDistanceImage[imagePosition]  = PackReblurRadianceHitDistance(max(payload.diffuseDenoiserRadiance, (float3)0.0) / diffuseDemodulationFactor, diffuseHitDistance, primaryViewZ, payload.primaryRoughness, true);
  specularRadianceHitDistanceImage[imagePosition] = PackReblurRadianceHitDistance(max(payload.specularDenoiserRadiance, (float3)0.0) / specularDemodulationFactor, specularHitDistance, primaryViewZ, payload.primaryRoughness, false);
}

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID    = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo   = pushConst.sceneInfoAddress.Get();
  const float2        pixelCenter = (float2)launchID + 0.5;

  RayDesc ray;

  ray.Origin    = sceneInfo.cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  // Payload
  // The path starts at the camera with unit throughput and an addressable spatiotemporal blue-noise stream; every denoiser field starts empty.

  PathPayload payload;

  payload.radiance                       = (float3)0.0;
  payload.throughput                     = (float3)1.0;
  payload.mediumAttenuation              = (float3)0.0;
  payload.mediumThickness                = 0.0;
  payload.lastSurfacePosition            = sceneInfo.cameraPosition;
  payload.lastSurfaceNormal              = float3(0.0, 1.0, 0.0);
  payload.lastDirectLightBsdfPdf         = 0.0;
  payload.seed                           = MakePathSampleStream(PackPathSamplePixel(launchID), pushConst.rngFrameNumber, 0u, kPTStreamNee);
  payload.depth                          = 0;
  payload.mediumActive                   = 0;
  payload.lastEnvironmentNeeActive       = 0;
  payload.lastEmissiveNeeActive          = 0;
  payload.hasLastBsdfSample              = 0;
  payload.primaryWorldPosition           = (float3)0.0;
  payload.primaryRoughness               = 0.0;
  payload.primaryShadingNormal           = (float3)0.0;
  payload.primaryBaseColor               = (float3)0.0;
  payload.primaryMetalness               = 0.0;
  payload.hasPrimarySurface              = 0;
  payload.diffuseDenoiserRadiance        = (float3)0.0;
  payload.diffuseDenoiserHitDistance     = 0.0;
  payload.specularDenoiserRadiance       = (float3)0.0;
  payload.specularDenoiserHitDistance    = 0.0;
  payload.hasDiffuseDenoiserHitDistance  = 0u;
  payload.hasSpecularDenoiserHitDistance = 0u;
  payload.firstBounceIsSpecular          = 0u;
  payload.awaitingFirstBounceHitDistance = 0u;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, payload);

  if((pushConst.flags & uint(PathTraceFlags::ePathTraceFlagWriteDenoiserSignals)) != 0u)
  {
    WriteDenoiserGuideBuffers(launchID, sceneInfo, payload);
    WriteDenoiserNoisySignals(launchID, payload);
  }

  // Resolve
  // Running average over accumulated frames when accumulation is on; otherwise this frame's sample is shown directly.

  const float3 sampleRadiance   = max(payload.radiance, (float3)0.0);
  float3       resolvedRadiance = sampleRadiance;

  if(IsFinalRadianceAccumulationEnabled() && pushConst.accumulatedFrames > 0)
  {
    const float3 previousAverage = accumulationImage[(int2)launchID].xyz;
    const float  historyWeight   = float(pushConst.accumulatedFrames);

    resolvedRadiance = (previousAverage * historyWeight + sampleRadiance) / (historyWeight + 1.0);
  }

  accumulationImage[(int2)launchID] = float4(resolvedRadiance, 1.0);
  outImage[(int2)launchID]          = float4(resolvedRadiance, 1.0);
}

[shader("miss")]
void rmissMain(inout PathPayload payload)
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();
  const float3        envColor  = SampleEnvironment(sceneInfo, WorldRayDirection());

  // MIS against the explicit environment sample the previous vertex took, when it took one.
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
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();
  const SurfaceData   surface   = LoadSurfaceData(attr);
  const float3        viewDir   = -WorldRayDirection();

  StorePrimarySurfaceForDenoiser(payload, surface);
  StoreFirstBounceHitDistance(payload, RayTCurrent());

  ApplyCurrentMediumAttenuation(payload, RayTCurrent());

  // Emission and direct light
  // Emission found by a BSDF-sampled ray is MIS-weighted against light sampling of the same triangle; the explicit environment and emissive samples follow.

  float emissiveMisWeight = 1.0;

  // Only when the previous vertex took an emissive light sample; otherwise nothing makes up the weight taken away here.
  if(payload.hasLastBsdfSample != 0 && payload.lastEmissiveNeeActive != 0 && SafeMax3(surface.emission) > 0.0)
  {
    const float lightPdf = EvaluateCurrentEmissiveHitPdf(sceneInfo, InstanceIndex(), PrimitiveIndex(), payload.lastSurfacePosition, surface.worldPosition);

    if(lightPdf > 0.0 && payload.lastDirectLightBsdfPdf > 0.0)
    {
      emissiveMisWeight = MisMixWeight(payload.lastDirectLightBsdfPdf, lightPdf);
    }
  }

  const float3 emissiveContribution = payload.throughput * surface.emission * emissiveMisWeight;

  payload.radiance += emissiveContribution;

  AccumulateDenoiserRadiance(payload, emissiveContribution);
  payload.seed = MakePathSampleStream(payload.seed.pixel, payload.seed.frame, payload.depth, kPTStreamNee);

  AccumulateEnvironmentDirectLight(payload, surface, sceneInfo, viewDir);

  payload.seed = MakePathSampleStream(payload.seed.pixel, payload.seed.frame, payload.depth, kPTStreamEmissive);

  AccumulateEmissiveDirectLight(payload, surface, sceneInfo, viewDir);

  if(payload.depth >= pushConst.maxBounces)
  {
    return;
  }

  // BSDF sample

  float3 bounceDir;
  float3 bsdfOverPdf         = (float3)0.0;
  float  directLightBsdfPdf  = 0.0;
  bool   isTransmissionEvent = false;
  uint   sampledLobeKind     = 0u;

  payload.seed = MakePathSampleStream(payload.seed.pixel, payload.seed.frame, payload.depth, kPTStreamBsdf);

  if(!SampleSurfaceBsdf(surface, viewDir, payload.seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
  {
    return;
  }

  const float geometricDot = dot(surface.geometricNormal, bounceDir);

  if(abs(geometricDot) <= 1.0e-6)
  {
    return;
  }

  float3 nextThroughput = payload.throughput * max(bsdfOverPdf, (float3)0.0);

  if(SafeMax3(nextThroughput) <= 0.0)
  {
    return;
  }

  if(payload.depth == 0u)
  {
    BeginFirstBounceDenoiserSignal(payload, sampledLobeKind);
  }

  payload.depth += 1;

  // Russian roulette from the second bounce onward, dividing survivors by their continuation probability so the estimate stays unbiased.
  if(payload.depth > 1)
  {
    const float continueProbability = clamp(SafeMax3(nextThroughput), 0.05, 0.95);

    payload.seed = MakePathSampleStream(payload.seed.pixel, payload.seed.frame, payload.depth - 1u, kPTStreamRoulette);

    if(NextRandom(payload.seed) > continueProbability)
    {
      return;
    }

    nextThroughput /= continueProbability;
  }

  // Continue the path
  // Update the medium, record what the next vertex's MIS needs, and trace the bounce.

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
      payload.mediumAttenuation = (float3)0.0;
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
  payload.lastEmissiveNeeActive    = CanSampleEmissiveDirectLight(surface, sceneInfo) ? 1u : 0u;
  payload.hasLastBsdfSample        = 1u;

  const float3 originNormal = SelectOffsetNormal(surface.geometricNormal, bounceDir);

  RayDesc bounceRay;

  bounceRay.Origin    = OffsetRay(surface.worldPosition, originNormal);
  bounceRay.Direction = bounceDir;
  bounceRay.TMin      = 0.001;
  bounceRay.TMax      = kRayTMax;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
}
