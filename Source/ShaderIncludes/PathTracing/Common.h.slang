#ifndef PATH_TRACING_COMMON_H
#define PATH_TRACING_COMMON_H

#ifndef PATH_TRACING_CUSTOM_GLOBALS
#include "ShaderIncludes/PathTracing/Globals.h.slang"
#endif

static const float kRayTMax                    = 1.0e32;
static const float kInvPi                      = 0.31830988618;
static const float kRayOriginFloor             = 1.0 / 32.0;
static const float kRayOriginFloatEps          = 1.0 / 65536.0;
static const float kRayOriginIntScale          = 256.0;

#ifndef PATH_TRACING_PAYLOAD_EXTRA_FIELDS
#define PATH_TRACING_PAYLOAD_EXTRA_FIELDS
#endif

struct PathPayload
{
  float3 radiance;
  float3 throughput;
  float3 mediumAttenuation;
  float  mediumThickness;
  float3 lastSurfacePosition;
  float3 lastSurfaceNormal;
  float  lastDirectLightBsdfPdf;
  uint   seed;
  uint   depth;
  uint   mediumActive;
  uint   lastEnvironmentNeeActive;
  uint   hasLastBsdfSample;
  float3 primaryWorldPosition;
  float  primaryRoughness;
  float3 primaryShadingNormal;
  float3 primaryBaseColor;
  float  primaryMetalness;
  uint   hasPrimarySurface;
  float3 diffuseDenoiserRadiance;
  float  diffuseDenoiserHitDistance;
  float3 specularDenoiserRadiance;
  float  specularDenoiserHitDistance;
  uint   hasDiffuseDenoiserHitDistance;
  uint   hasSpecularDenoiserHitDistance;
  uint   firstBounceIsSpecular;
  uint   awaitingFirstBounceHitDistance;
  PATH_TRACING_PAYLOAD_EXTRA_FIELDS
};

struct ShadowPayload
{
  uint visible;
};

struct SurfaceData
{
  float3 worldPosition;
  float3 shadingNormal;
  float3 geometricNormal;
  float3 tangent;
  float3 bitangent;
  float2 texCoord;
  float3 albedo;
  float3 emission;
  float  metallic;
  float  roughness;
  float  specular;
  float  specularTint;
  float  subsurface;
  float  anisotropy;
  float3 attenuationColor;
  float  transmission;
  float  attenuationDistance;
  float  volumeThickness;
  float  refractionIndex;
  float  clearcoat;
  float  clearcoatRoughness;
  float3 sheenColor;
  float  sheenRoughness;
  uint   isFrontFace;
};

void StorePrimarySurfaceForDenoiser(inout PathPayload payload, SurfaceData surface)
{
  if(payload.hasPrimarySurface != 0u)
  {
    return;
  }

  payload.primaryWorldPosition = surface.worldPosition;
  payload.primaryRoughness     = surface.roughness;
  payload.primaryShadingNormal = surface.shadingNormal;
  payload.primaryBaseColor     = surface.albedo;
  payload.primaryMetalness     = surface.metallic;
  payload.hasPrimarySurface    = 1u;
}

float3 ComputeDenoiserMotionVector(float2 pixelCenter, float3 worldPosition, GltfSceneInfo sceneInfo)
{
  const float2 currentUv = pixelCenter / sceneInfo.viewportSize;

  const float4 previousClipPosition = mul(float4(worldPosition, 1.0), sceneInfo.prevViewProjMatrix);
  if(previousClipPosition.w <= 0.0)
  {
    return float3(0.0, 0.0, 0.0);
  }

  const float2 previousUv = previousClipPosition.xy / previousClipPosition.w * 0.5 + 0.5;
  const float currentViewZ = mul(float4(worldPosition, 1.0), sceneInfo.viewMatrix).z;
  const float previousViewZ = mul(float4(worldPosition, 1.0), sceneInfo.prevViewMatrix).z;
  return float3(previousUv - currentUv, previousViewZ - currentViewZ);
}

bool IsAccumulationEnabled()
{
  return (pushConst.flags & uint(PathTraceFlags::ePathTraceFlagAccumulate)) != 0;
}

bool IsSpecularDenoiserLobe(uint sampledLobeKind)
{
  return sampledLobeKind != 0u;
}

void AccumulateDenoiserRadiance(inout PathPayload payload, float3 radiance)
{
  if(payload.hasPrimarySurface == 0u)
  {
    return;
  }

  if(payload.depth == 0u || payload.firstBounceIsSpecular == 0u)
  {
    payload.diffuseDenoiserRadiance += radiance;
    return;
  }

  payload.specularDenoiserRadiance += radiance;
}

void BeginFirstBounceDenoiserSignal(inout PathPayload payload, uint sampledLobeKind)
{
  if(payload.hasPrimarySurface == 0u)
  {
    return;
  }

  payload.firstBounceIsSpecular          = IsSpecularDenoiserLobe(sampledLobeKind) ? 1u : 0u;
  payload.awaitingFirstBounceHitDistance = 1u;
}

void StoreFirstBounceHitDistance(inout PathPayload payload, float hitDistance)
{
  if(payload.awaitingFirstBounceHitDistance == 0u)
  {
    return;
  }

  const float clampedHitDistance = max(hitDistance, 0.0);
  if(payload.firstBounceIsSpecular != 0u)
  {
    payload.specularDenoiserHitDistance    = clampedHitDistance;
    payload.hasSpecularDenoiserHitDistance = 1u;
  }
  else
  {
    payload.diffuseDenoiserHitDistance    = clampedHitDistance;
    payload.hasDiffuseDenoiserHitDistance = 1u;
  }

  payload.awaitingFirstBounceHitDistance = 0u;
}

bool IsFinalRadianceAccumulationEnabled()
{
  return IsAccumulationEnabled();
}
#endif









