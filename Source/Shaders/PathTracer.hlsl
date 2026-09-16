// Reference path tracer
// Plain unidirectional path tracer with next event estimation. It runs the shared path loop (PathLoop.hlsli) with every hook at its default, which is the same code ReSTIR PT initial sampling runs with its hooks defined.
// Radiance is summed directly into the path state, so this is the estimator the resampled renderer is validated against. It also writes NRD's guide buffers and demodulated radiance signals when denoising is on.

#include <Common/ShaderTypes.h>
#include "ShaderIncludes/Random.hlsli"
#include "ShaderIncludes/Sky.hlsli"
#include "ShaderIncludes/Camera.hlsli"
#include "ShaderIo.h"
#include "NRD.hlsli"
#include "ShaderIncludes/DenoiserInputs.hlsli"
#include "ShaderIncludes/PathTracing/Globals.hlsli"

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"
#include "ShaderIncludes/PathTracing/Volume.hlsli"
#include "ShaderIncludes/PathTracing/Hdri.hlsli"
#include "ShaderIncludes/PathTracing/Disney.hlsli"
#include "ShaderIncludes/PathTracing/Intersection.hlsli"
#include "ShaderIncludes/PathTracing/Lights.hlsli"
#include "ShaderIncludes/PathTracing/PathLoop.hlsli"
#include "ShaderIncludes/PathTracing/PathRayEntryPoints.hlsli"

// Denoiser constants
// kInvalidDenoiserViewZ marks a pixel with no primary hit.
// The demodulation factors are floored so dividing radiance by them cannot blow up on black or non-reflective materials.

static const float  kInvalidDenoiserViewZ          = 1.0e32;
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
void ComputeDenoiserMaterialFactors(PathState path, out float3 diffuseFactor, out float3 specularFactor)
{
  if(path.hasPrimarySurface == 0u)
  {
    diffuseFactor  = float3(1.0, 1.0, 1.0);
    specularFactor = float3(1.0, 1.0, 1.0);

    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();
  const float3        viewDir   = normalize(sceneInfo.cameraPosition - path.primaryWorldPosition);
  const float3        rf0       = lerp(float3(0.04, 0.04, 0.04), path.primaryBaseColor, path.primaryMetalness);

  // Demodulation factors
  // NRD recommends filtering material-independent radiance. The preintegrated specular factor comes from NRD_MaterialFactors and is stored for NrdCompose to replay verbatim.
  // The diffuse factor must be the one NrdCompose recomputes from base colour and metalness, not NRD_MaterialFactors' diffuse output: compose multiplies back by its own formula, so dividing by a different quantity would scale the denoised diffuse signal. ReSTIRPTFinalShading uses the same pair for the same reason.

  float3 unusedDiffuseFactor;

  NRD_MaterialFactors(path.primaryShadingNormal, viewDir, path.primaryBaseColor, rf0, path.primaryRoughness, unusedDiffuseFactor, specularFactor);

  diffuseFactor  = max(saturate(path.primaryBaseColor) * (1.0 - saturate(path.primaryMetalness)), kMinDiffuseDemodulationFactor);
  specularFactor = max(specularFactor, kMinSpecularDemodulationFactor);
}

// Writes the per-pixel geometry NRD reprojects and filters with. A pixel with no primary hit still gets deterministic values, so NRD never reads an uninitialized texel.
void WriteDenoiserGuideBuffers(uint2 pixelPosition, GltfSceneInfo sceneInfo, PathState path)
{
  if(path.hasPrimarySurface != 0u)
  {
    const float  viewZ  = mul(float4(path.primaryWorldPosition, 1.0), sceneInfo.viewMatrix).z;
    const float3 motion = ComputeDenoiserMotionVector((float2)pixelPosition + 0.5, path.primaryWorldPosition, sceneInfo);

    motionVectorsImage[(int2)pixelPosition]      = float4(motion, 0.0);
    normalRoughnessImage[(int2)pixelPosition]    = PackNrdNormalRoughness(path.primaryShadingNormal, path.primaryRoughness);
    baseColorMetalnessImage[(int2)pixelPosition] = PackBaseColorMetalness(path.primaryBaseColor, path.primaryMetalness);
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
void WriteDenoiserNoisySignals(uint2 pixelPosition, PathState path)
{
  const int2 imagePosition = (int2)pixelPosition;

  if(path.hasPrimarySurface == 0u)
  {
    diffuseRadianceHitDistanceImage[imagePosition]  = float4(0.0, 0.0, 0.0, 0.0);
    specularRadianceHitDistanceImage[imagePosition] = float4(0.0, 0.0, 0.0, 0.0);
    specularDemodulationFactorImage[imagePosition]  = float4(1.0, 1.0, 1.0, 1.0);

    return;
  }

  const GltfSceneInfo sceneInfo           = pushConst.sceneInfoAddress.Get();
  const float         primaryViewZ        = mul(float4(path.primaryWorldPosition, 1.0), sceneInfo.viewMatrix).z;
  const float         diffuseHitDistance  = path.hasDiffuseDenoiserHitDistance != 0u ? path.diffuseDenoiserHitDistance : 0.0;
  const float         specularHitDistance = path.hasSpecularDenoiserHitDistance != 0u ? path.specularDenoiserHitDistance : 0.0;

  float3 diffuseDemodulationFactor;
  float3 specularDemodulationFactor;

  ComputeDenoiserMaterialFactors(path, diffuseDemodulationFactor, specularDemodulationFactor);

  specularDemodulationFactorImage[imagePosition] = float4(specularDemodulationFactor, 1.0);

  // Demodulated, then clamped, in RTXPT's order: the clamp limits what NRD accumulates, so it acts on the signal NRD actually sees.
  const float3 diffuseRadiance  = ClampDenoiserRadiance(max(path.diffuseDenoiserRadiance, (float3)0.0) / diffuseDemodulationFactor, pushConst.denoiserRadianceClamp);
  const float3 specularRadiance = ClampDenoiserRadiance(max(path.specularDenoiserRadiance, (float3)0.0) / specularDemodulationFactor, pushConst.denoiserRadianceClamp);

  diffuseRadianceHitDistanceImage[imagePosition]  = PackReblurRadianceHitDistance(diffuseRadiance, diffuseHitDistance, primaryViewZ, path.primaryRoughness, true);
  specularRadianceHitDistanceImage[imagePosition] = PackReblurRadianceHitDistance(specularRadiance, specularHitDistance, primaryViewZ, path.primaryRoughness, false);
}

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID  = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();

  // Path
  // The path leaves the camera through the pixel center, and the shared loop traces and shades every bounce.

  PathState path = BeginPath(launchID, sceneInfo);

  TracePath(path, MakeCameraRay(launchID, sceneInfo), sceneInfo);

  if((pushConst.flags & uint(PathTraceFlags::ePathTraceFlagWriteDenoiserSignals)) != 0u)
  {
    WriteDenoiserGuideBuffers(launchID, sceneInfo, path);
    WriteDenoiserNoisySignals(launchID, path);
  }

  // Resolve
  // Running average over accumulated frames when accumulation is on; otherwise this frame's sample is shown directly.

  const float3 sampleRadiance   = max(path.radiance, (float3)0.0);
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
