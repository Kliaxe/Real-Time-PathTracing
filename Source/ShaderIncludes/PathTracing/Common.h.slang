#ifndef PATH_TRACING_COMMON_H
#define PATH_TRACING_COMMON_H

#ifndef PATH_TRACING_CUSTOM_GLOBALS
#include "ShaderIncludes/PathTracing/Globals.h.slang"
#endif

static const float kRayTMax                    = 1.0e32;
static const float kDenoiserFarHitDistance     = 65504.0;
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
  // NRD's REBLUR path adds mv.xy directly to pixel UV, so XY is stored as a UV delta.
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

// Observation hook for every radiance contribution the path tracer produces.
//
// A plain path tracer sums contributions into payload.radiance. ReSTIR PT instead
// treats each one as a candidate path and streams it through RIS, so it needs to
// see them individually rather than as a sum. Defining this macro lets a shader
// observe each contribution without changing how the path itself is sampled.
//
// The hook is additive, not a replacement: payload.radiance keeps accumulating
// normally, so a ReSTIR PT pass carries the plain path-traced answer for the same
// sample alongside its reservoir. That makes the two directly comparable in one
// pass, which is how the resampled result is validated against the reference.
#ifndef PATH_TRACING_ON_CONTRIBUTION
#define PATH_TRACING_ON_CONTRIBUTION(payload, contribution)
#endif

// As above, but for a site whose contribution is not already an unbiased estimate
// on its own - it carries a separate weight. Kept distinct from the unweighted
// hook because an observer usually needs the two factors apart: the contribution
// is what another domain could reconstruct, the weight is what only this sample
// knows.
#ifndef PATH_TRACING_ON_CONTRIBUTION_WEIGHTED
#define PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(payload, contribution, candidateWeight)
#endif

// Announces an emissive next-event-estimation sample just before its contribution
// is accumulated. Empty for a plain path tracer; ReSTIR PT uses it to adopt the
// light vertex as a reconnection anchor, which is the only way a path too short to
// contain an interior vertex can be shifted to another pixel.
//
// lightIndex indexes the emissive triangle list, so the sampled point can be
// rebuilt later from the light itself rather than from a stored world position.
#ifndef PATH_TRACING_ON_NEE_EMISSIVE
#define PATH_TRACING_ON_NEE_EMISSIVE(payload, lightIndex, barycentrics, radiance, lightPdf, distance)
#endif

// Single funnel for radiance contributions. Every site that adds light to a path
// must route through here so the hook above cannot miss one.
void AccumulatePathContribution(inout PathPayload payload, float3 contribution)
{
  payload.radiance += contribution;
  AccumulateDenoiserRadiance(payload, contribution);
  PATH_TRACING_ON_CONTRIBUTION(payload, contribution);
}

// Same funnel for a site that produced its contribution through a sampling process
// with its own unbiased contribution weight, such as RIS over several candidates.
// The radiance sum takes the product, which is the estimate; the hook receives both
// factors, because a resampler stores them separately.
void AccumulatePathContributionWithWeight(inout PathPayload payload, float3 contribution, float candidateWeight)
{
  const float3 estimate = contribution * candidateWeight;
  payload.radiance += estimate;
  AccumulateDenoiserRadiance(payload, estimate);
  PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(payload, contribution, candidateWeight);
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

void StoreDiffuseDenoiserHitDistanceIfMissing(inout PathPayload payload, float hitDistance)
{
  if(payload.hasPrimarySurface == 0u || payload.depth != 0u || payload.hasDiffuseDenoiserHitDistance != 0u)
  {
    return;
  }

  // Direct-light next-event samples have no secondary ray hit. Give REBLUR the
  // light distance instead of a zero fallback so its blur radius is not overly strict.
  payload.diffuseDenoiserHitDistance    = max(hitDistance, 0.0);
  payload.hasDiffuseDenoiserHitDistance = 1u;
}

bool IsFinalRadianceAccumulationEnabled()
{
  return IsAccumulationEnabled();
}
#endif









