#ifndef PATH_TRACING_COMMON_H
#define PATH_TRACING_COMMON_H

// ReSTIR PT defines PATH_TRACING_CUSTOM_GLOBALS and declares its own resources under the same names.
#ifndef PATH_TRACING_CUSTOM_GLOBALS
#include "ShaderIncludes/PathTracing/Globals.hlsli"
#endif

#include "ShaderIncludes/PathTracing/Sampling.hlsli"

// Constants
// kDenoiserFarHitDistance is the largest half-float value, reported to REBLUR for hits at infinity such as the environment.
// The three kRayOrigin* values are the parameters of OffsetRay's self-intersection offset (Ray Tracing Gems chapter 6): the near-origin threshold, the fixed float epsilon used below it, and the scale of the integer ULP offset.

static const float kRayTMax                    = 1.0e32;
static const float kDenoiserFarHitDistance     = 65504.0;
static const float kInvPi                      = 0.31830988618;
static const float kRayOriginFloor             = 1.0 / 32.0;
static const float kRayOriginFloatEps          = 1.0 / 65536.0;
static const float kRayOriginIntScale          = 256.0;

// A shader that needs extra per-path state defines this before including the header; ReSTIR PT uses it to carry its reservoir.
#ifndef PATH_TRACING_STATE_EXTRA_FIELDS
#define PATH_TRACING_STATE_EXTRA_FIELDS
#endif

// PathState
// Per-path state of the path loop in PathLoop.hlsli, held in a ray generation local for the whole path.
// It never travels through TraceRay: a ray payload is copied into and out of every trace, and carrying this much state on every bounce cost more than tracing the ray itself. Path rays carry a PathHitRecord instead.
// Besides the running estimate it remembers the previous scattering vertex, so light hits can be MIS weighted against next-event estimation, and the primary-surface data and split radiance the NRD denoiser needs.

struct PathState
{
  // Sum of every contribution along the path; the pixel's radiance estimate.
  float3 radiance;
  // Product of BSDF-over-PDF, Russian roulette, and medium attenuation factors up to the current vertex.
  float3 throughput;
  // Extinction coefficient of the medium the path is inside, valid while mediumActive is set.
  float3 mediumAttenuation;
  // Authored volume thickness, used as the medium distance when a path escapes to the environment from inside a medium.
  float  mediumThickness;
  // Previous scattering vertex, from which an emissive hit's light PDF is evaluated for MIS.
  float3 lastSurfacePosition;
  // Previous vertex's shading normal, the receiver normal for the environment light PDF on an escape.
  float3 lastSurfaceNormal;
  // Direct-light-compatible BSDF PDF of the previous bounce. Zero unless the broad group was sampled.
  float  lastDirectLightBsdfPdf;
  // Addressable samples shared by both tracers and regenerated from the original path during replay.
  PathSampleStream seed;
  // Bounces taken so far. Zero while shading the primary hit.
  uint   depth;
  // Nonzero while the path travels inside an absorbing medium.
  uint   mediumActive;
  // Nonzero if the previous vertex sampled the environment explicitly for the event it scattered through, so an escape must be MIS weighted.
  uint   lastEnvironmentNeeActive;
  // Nonzero if the previous vertex took an emissive light sample, so an emissive hit must be MIS weighted. A vertex that skips emissive NEE leaves it clear, and the hit keeps its full weight because no light sample shares its energy.
  uint   lastEmissiveNeeActive;
  // Zero for the camera ray. Light reached directly from the camera takes full weight because no light sample competes with it.
  uint   hasLastBsdfSample;
  // Primary hit position for the denoiser's view depth and motion vectors.
  float3 primaryWorldPosition;
  // Primary hit roughness for the NRD normal/roughness guide.
  float  primaryRoughness;
  // Primary hit shading normal for the NRD normal/roughness guide.
  float3 primaryShadingNormal;
  // Primary hit base color, used for guide output and material demodulation.
  float3 primaryBaseColor;
  // Primary hit metalness, used for guide output and material demodulation.
  float  primaryMetalness;
  // Set once at the first hit. Contributions only reach the denoiser signals after it is set.
  uint   hasPrimarySurface;
  // Radiance attributed to the diffuse denoiser signal.
  float3 diffuseDenoiserRadiance;
  // First-bounce hit distance for the diffuse signal, valid when hasDiffuseDenoiserHitDistance is set.
  float  diffuseDenoiserHitDistance;
  // Radiance attributed to the specular denoiser signal.
  float3 specularDenoiserRadiance;
  // First-bounce hit distance for the specular signal, valid when hasSpecularDenoiserHitDistance is set.
  float  specularDenoiserHitDistance;
  // Whether diffuseDenoiserHitDistance has been written.
  uint   hasDiffuseDenoiserHitDistance;
  // Whether specularDenoiserHitDistance has been written.
  uint   hasSpecularDenoiserHitDistance;
  // Nonzero when the primary bounce sampled a non-broad lobe, routing later radiance to the specular signal.
  uint   firstBounceIsSpecular;
  // Set when the primary bounce ray is launched and cleared once its hit or escape records the distance.
  uint   awaitingFirstBounceHitDistance;
  // Shader-specific extensions; see the macro above.
  PATH_TRACING_STATE_EXTRA_FIELDS
};

// PathHitRecord
// The ray payload of path rays, and all their closest-hit and miss shaders write: which triangle was hit and where along the ray, not what the surface there is.
// Ray generation rebuilds the full SurfaceData from it with LoadSurfaceDataFromHit. Front-facing is not recorded because the loader derives it from the ray direction, which ray generation already holds.

struct PathHitRecord
{
  // InstanceIndex() of the hit, which selects the instance transform, mesh, and material.
  uint   instanceIndex;
  // PrimitiveIndex() of the hit triangle within the instance's mesh.
  uint   primitiveIndex;
  // DXR barycentrics: the weights of the triangle's second and third vertices.
  float2 barycentrics;
  // RayTCurrent() at the hit: the segment length for medium attenuation and the denoiser's hit distance.
  float  hitDistance;
  // One when closest hit ran. The miss shader writes zero, and every other field is then undefined.
  uint   hasHit;
};

// ShadowPayload
// Payload for visibility rays. Closest-hit is skipped for these, so only the shadow miss shader writes to it.

struct ShadowPayload
{
  // Starts at zero and is set to one by the shadow miss shader when nothing blocked the ray.
  uint visible;
};

// SurfaceData
// A hit point fully resolved from glTF geometry, textures, and material extensions.
// LoadSurfaceDataFromHit fills it once per hit so the BSDF and light sampling code never touch raw scene buffers.

struct SurfaceData
{
  // Hit position in world space.
  float3 worldPosition;
  // Interpolated, normal-mapped normal, face-forwarded against the incoming ray.
  float3 shadingNormal;
  // Triangle normal from world-space vertices, face-forwarded against the incoming ray.
  float3 geometricNormal;
  // Tangent axis of the shading frame, orthogonal to shadingNormal.
  float3 tangent;
  // Bitangent axis of the shading frame, carrying the authored handedness.
  float3 bitangent;
  // Interpolated first texture coordinate set.
  float2 texCoord;
  // Base color after texturing.
  float3 albedo;
  // Emitted radiance after the emissive texture.
  float3 emission;
  // Metalness in [0, 1].
  float  metallic;
  // Perceptual roughness, floored at 0.045.
  float  roughness;
  // KHR_materials_specular strength.
  float  specular;
  // Scalar specular tint amount derived from the specular color.
  float  specularTint;
  // Blend toward the Hanrahan-Krueger subsurface diffuse approximation.
  float  subsurface;
  // Anisotropy strength in [0, 1].
  float  anisotropy;
  // Volume color reached after attenuationDistance of travel.
  float3 attenuationColor;
  // KHR_materials_transmission factor.
  float  transmission;
  // Distance at which transmittance equals attenuationColor. Zero disables absorption.
  float  attenuationDistance;
  // KHR_materials_volume thickness.
  float  volumeThickness;
  // Index of refraction, floored at 1.01.
  float  refractionIndex;
  // Clearcoat layer strength.
  float  clearcoat;
  // Perceptual clearcoat roughness, floored at 0.045.
  float  clearcoatRoughness;
  // Sheen reflectance color.
  float3 sheenColor;
  // Sheen roughness for the Charlie distribution.
  float  sheenRoughness;
  // One when the ray hits the side the triangle's winding faces, meaning it is entering the surface.
  uint   isFrontFace;
};

// Records the first surface along the path as the denoiser's guide data. Later hits leave it untouched.
void StorePrimarySurfaceForDenoiser(inout PathState path, SurfaceData surface)
{
  if(path.hasPrimarySurface != 0u)
  {
    return;
  }

  path.primaryWorldPosition = surface.worldPosition;
  path.primaryRoughness     = surface.roughness;
  path.primaryShadingNormal = surface.shadingNormal;
  path.primaryBaseColor     = surface.albedo;
  path.primaryMetalness     = surface.metallic;
  path.hasPrimarySurface    = 1u;
}

// Motion of a world position between the previous and current frame, in NRD's convention.
float3 ComputeDenoiserMotionVector(float2 pixelCenter, float3 worldPosition, GltfSceneInfo sceneInfo)
{
  // NRD's REBLUR path adds mv.xy directly to pixel UV, so XY is stored as a UV delta.
  const float2 currentUv = pixelCenter / sceneInfo.viewportSize;

  const float4 previousClipPosition = mul(float4(worldPosition, 1.0), sceneInfo.prevViewProjMatrix);

  // A point behind the previous camera has no valid reprojection.
  if(previousClipPosition.w <= 0.0)
  {
    return float3(0.0, 0.0, 0.0);
  }

  // No Y flip, matching ReconstructWorldDirectionFromPixel's pixel-to-NDC mapping. Z carries the change in view depth.
  const float2 previousUv   = previousClipPosition.xy / previousClipPosition.w * 0.5 + 0.5;
  const float currentViewZ  = mul(float4(worldPosition, 1.0), sceneInfo.viewMatrix).z;
  const float previousViewZ = mul(float4(worldPosition, 1.0), sceneInfo.prevViewMatrix).z;

  return float3(previousUv - currentUv, previousViewZ - currentViewZ);
}

bool IsAccumulationEnabled()
{
  return (pushConst.flags & uint(PathTraceFlags::ePathTraceFlagAccumulate)) != 0;
}

// Lobe kind 0 is the broad diffuse/sheen group. Glossy reflection, glass, and clearcoat all feed the specular signal.
bool IsSpecularDenoiserLobe(uint sampledLobeKind)
{
  return sampledLobeKind != 0u;
}

// Routes a contribution into the diffuse or specular denoiser signal.
void AccumulateDenoiserRadiance(inout PathState path, float3 radiance)
{
  // Radiance without a primary surface (camera rays that miss) is not part of either signal.
  if(path.hasPrimarySurface == 0u)
  {
    return;
  }

  // Light gathered at the primary vertex itself (its emission and broad-group NEE) is diffuse. Everything later follows the first bounce's lobe.
  if(path.depth == 0u || path.firstBounceIsSpecular == 0u)
  {
    path.diffuseDenoiserRadiance += radiance;
    return;
  }

  path.specularDenoiserRadiance += radiance;
}

// Contribution sites
// Which estimator in the path loop publishes the next contribution, announced through PATH_TRACING_ON_CONTRIBUTION_SITE (PathLoop.hlsli) just before it.
// The contribution hooks below cannot see their caller; ReSTIR PT records the site as the endpoint kind of the path it selects.

static const uint kPathContributionSiteEmissionHit    = 0u;
static const uint kPathContributionSiteEnvironmentNee = 1u;
static const uint kPathContributionSiteEmissiveNee    = 2u;
static const uint kPathContributionSiteEscape         = 3u;

// Contribution hook
// Observation hook for every radiance contribution the path tracer produces.
// A plain path tracer sums contributions into path.radiance. ReSTIR PT instead treats each one as a candidate path and streams it through RIS, so it needs to see them individually rather than as a sum. Defining this macro lets a shader observe each contribution without changing how the path itself is sampled.
// The hook is additive, not a replacement: path.radiance keeps accumulating normally, so a ReSTIR PT pass carries the plain path-traced answer for the same sample alongside its reservoir. That makes the two directly comparable in one pass, which is how the resampled result is validated against the reference.

#ifndef PATH_TRACING_ON_CONTRIBUTION
#define PATH_TRACING_ON_CONTRIBUTION(path, contribution)
#endif

// Weighted contribution hook
// As above, but for a site whose contribution is not already an unbiased estimate on its own: it carries a separate weight.
// Kept distinct from the unweighted hook because an observer usually needs the two factors apart. The contribution is what another domain could reconstruct; the weight is what only this sample knows.

#ifndef PATH_TRACING_ON_CONTRIBUTION_WEIGHTED
#define PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(path, contribution, candidateWeight)
#endif

// Emissive NEE hook
// Announces an emissive next-event-estimation sample just before its contribution is accumulated. Empty for a plain path tracer; ReSTIR PT uses it to adopt the light vertex as a reconnection anchor, which is the only way a path too short to contain an interior vertex can be shifted to another pixel.
// lightIndex indexes the emissive triangle list, so the sampled point can be rebuilt later from the light itself rather than from a stored world position.

#ifndef PATH_TRACING_ON_NEE_EMISSIVE
#define PATH_TRACING_ON_NEE_EMISSIVE(path, lightIndex, barycentrics, radiance, lightPdf, distance)
#endif

// Single funnel for radiance contributions.
// Every site that adds light to a path must route through here so the hook above cannot miss one.
void AccumulatePathContribution(inout PathState path, float3 contribution)
{
  path.radiance += contribution;

  AccumulateDenoiserRadiance(path, contribution);

  PATH_TRACING_ON_CONTRIBUTION(path, contribution);
}

// Same funnel for a site that produced its contribution through a sampling process with its own unbiased contribution weight, such as RIS over several candidates.
// The radiance sum takes the product, which is the estimate. The hook receives both factors, because a resampler stores them separately.
void AccumulatePathContributionWithWeight(inout PathState path, float3 contribution, float candidateWeight)
{
  const float3 estimate = contribution * candidateWeight;

  path.radiance += estimate;

  AccumulateDenoiserRadiance(path, estimate);

  PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(path, contribution, candidateWeight);
}

// Called when the primary vertex launches its bounce ray: fixes which signal later radiance feeds and arms the hit-distance capture.
void BeginFirstBounceDenoiserSignal(inout PathState path, uint sampledLobeKind)
{
  if(path.hasPrimarySurface == 0u)
  {
    return;
  }

  path.firstBounceIsSpecular          = IsSpecularDenoiserLobe(sampledLobeKind) ? 1u : 0u;
  path.awaitingFirstBounceHitDistance = 1u;
}

// Records the length of the primary bounce segment for the signal that bounce feeds. Only the first call after arming takes effect.
// It overwrites any diffuse fallback distance stored earlier by StoreDiffuseDenoiserHitDistanceIfMissing.
void StoreFirstBounceHitDistance(inout PathState path, float hitDistance)
{
  if(path.awaitingFirstBounceHitDistance == 0u)
  {
    return;
  }

  const float clampedHitDistance = max(hitDistance, 0.0);

  if(path.firstBounceIsSpecular != 0u)
  {
    path.specularDenoiserHitDistance    = clampedHitDistance;
    path.hasSpecularDenoiserHitDistance = 1u;
  }
  else
  {
    path.diffuseDenoiserHitDistance    = clampedHitDistance;
    path.hasDiffuseDenoiserHitDistance = 1u;
  }

  path.awaitingFirstBounceHitDistance = 0u;
}

// Supplies a diffuse hit distance from a primary-vertex light sample when nothing has provided one yet.
void StoreDiffuseDenoiserHitDistanceIfMissing(inout PathState path, float hitDistance)
{
  if(path.hasPrimarySurface == 0u || path.depth != 0u || path.hasDiffuseDenoiserHitDistance != 0u)
  {
    return;
  }

  // Direct-light next-event samples have no secondary ray hit. Give REBLUR the light distance instead of a zero fallback so its blur radius is not overly strict.
  path.diffuseDenoiserHitDistance    = max(hitDistance, 0.0);
  path.hasDiffuseDenoiserHitDistance = 1u;
}

// Whether the ray generation shader blends this frame into the accumulation history. Currently the same as IsAccumulationEnabled.
bool IsFinalRadianceAccumulationEnabled()
{
  return IsAccumulationEnabled();
}
#endif
