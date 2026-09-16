#ifndef PATH_TRACING_PATH_LOOP_H
#define PATH_TRACING_PATH_LOOP_H

// Path loop
// The path tracing estimator shared by the reference path tracer and ReSTIR PT initial sampling, written once.
// Ray generation traces every segment from a loop and shades each vertex itself; closest hit and miss only fill a PathHitRecord. The path state stays in a ray generation local, so each trace copies a small hit record instead of the whole path, and the pipeline needs one level of recursion however long the path is.
// A shader customizes the loop through the hook macros below, defined before including this header. With every hook left at its default the loop is exactly the reference estimator.

#include "ShaderIncludes/Camera.hlsli"
#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"
#include "ShaderIncludes/PathTracing/Volume.hlsli"
#include "ShaderIncludes/PathTracing/Hdri.hlsli"
#include "ShaderIncludes/PathTracing/Disney.hlsli"
#include "ShaderIncludes/PathTracing/Intersection.hlsli"
#include "ShaderIncludes/PathTracing/Lights.hlsli"

// Surface hook
// Sees each vertex's resolved surface, and the hit record it was resolved from, before any of its light is gathered. ReSTIR PT captures the primary-surface terms and the primary hit identity its reuse passes need.

#ifndef PATH_TRACING_ON_SURFACE
#define PATH_TRACING_ON_SURFACE(path, surface, hit, sceneInfo)
#endif

// Contribution site hook
// Announces which estimator publishes the next contribution; see kPathContributionSite* in Common.hlsli.

#ifndef PATH_TRACING_ON_CONTRIBUTION_SITE
#define PATH_TRACING_ON_CONTRIBUTION_SITE(path, site)
#endif

// Emissive NEE estimator
// The emissive light sample taken at each vertex. The reference draws one light from the scene-wide power CDF; ReSTIR PT may substitute its RIS estimator of the same integral.
// hit names the vertex's own triangle, which must not be sampled as its light.

#ifndef PATH_TRACING_ACCUMULATE_EMISSIVE_NEE
#define PATH_TRACING_ACCUMULATE_EMISSIVE_NEE(path, surface, hit, sceneInfo, viewDir, vertexDepth) AccumulateEmissiveDirectLight(path, surface, hit.instanceIndex, hit.primitiveIndex, sceneInfo, viewDir)
#endif

// Roulette gate
// Whether Russian roulette may end the path after the bounce just sampled, tested with path.depth already counting that bounce. The reference starts at the second bounce; ReSTIR PT makes the start a setting.

#ifndef PATH_TRACING_ROULETTE_ACTIVE
#define PATH_TRACING_ROULETTE_ACTIVE(path) ((path).depth > 1u)
#endif

// BSDF sample hook
// Sees each bounce that survived roulette before the path moves past its vertex: path still describes the predecessor, and nextThroughput is what the bounce will carry. ReSTIR PT runs its reconnection vertex search here.

#ifndef PATH_TRACING_ON_BSDF_SAMPLE
#define PATH_TRACING_ON_BSDF_SAMPLE(path, surface, hit, viewDir, bounceDir, sampledLobeKind, vertexDepth, nextThroughput, sceneInfo)
#endif

// A path leaving the camera with unit throughput and an addressable blue-noise stream, with every denoiser field empty.
// Fields added through PATH_TRACING_STATE_EXTRA_FIELDS are left for the calling shader to initialize.
PathState BeginPath(uint2 launchID, GltfSceneInfo sceneInfo)
{
  PathState path;

  path.radiance                       = (float3)0.0;
  path.throughput                     = (float3)1.0;
  path.mediumAttenuation              = (float3)0.0;
  path.mediumThickness                = 0.0;
  path.lastSurfacePosition            = sceneInfo.cameraPosition;
  path.lastSurfaceNormal              = float3(0.0, 1.0, 0.0);
  path.lastDirectLightBsdfPdf         = 0.0;
  path.seed                           = MakePathSampleStream(PackPathSamplePixel(launchID), pushConst.rngFrameNumber, 0u, kPTStreamNee);
  path.depth                          = 0;
  path.mediumActive                   = 0;
  path.lastEnvironmentNeeActive       = 0;
  path.lastEmissiveNeeActive          = 0;
  path.hasLastBsdfSample              = 0;
  path.primaryWorldPosition           = (float3)0.0;
  path.primaryRoughness               = 0.0;
  path.primaryShadingNormal           = (float3)0.0;
  path.primaryBaseColor               = (float3)0.0;
  path.primaryMetalness               = 0.0;
  path.hasPrimarySurface              = 0;
  path.diffuseDenoiserRadiance        = (float3)0.0;
  path.diffuseDenoiserHitDistance     = 0.0;
  path.specularDenoiserRadiance       = (float3)0.0;
  path.specularDenoiserHitDistance    = 0.0;
  path.hasDiffuseDenoiserHitDistance  = 0u;
  path.hasSpecularDenoiserHitDistance = 0u;
  path.firstBounceIsSpecular          = 0u;
  path.awaitingFirstBounceHitDistance = 0u;

  return path;
}

// Camera ray through this frame's sample position in the pixel: the center, unless DLSS Ray Reconstruction has jittered it.
RayDesc MakeCameraRay(uint2 launchID, GltfSceneInfo sceneInfo)
{
  RayDesc ray;

  ray.Origin    = sceneInfo.cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel(GetPixelSamplePosition(launchID, sceneInfo), sceneInfo);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  return ray;
}

// Traces one path segment through hit group 0 and miss shader 0, which only fill the hit record.
PathHitRecord TracePathSegment(RayDesc ray)
{
  PathHitRecord hit;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, hit);

  return hit;
}

// Environment light collected by a segment that left the scene.
void AccumulatePathEscape(inout PathState path, float3 direction, GltfSceneInfo sceneInfo)
{
  const float3 envColor = SampleEnvironment(sceneInfo, direction);

  // MIS against the explicit environment sample the previous vertex took, when it took one.
  float misWeight = 1.0;

  if(path.hasLastBsdfSample != 0 && path.lastEnvironmentNeeActive != 0)
  {
    const float lightPdf = EvaluateEnvironmentLightPdf(sceneInfo, path.lastSurfaceNormal, direction);

    if(lightPdf > 0.0 && path.lastDirectLightBsdfPdf > 0.0)
    {
      misWeight = MisMixWeight(path.lastDirectLightBsdfPdf, lightPdf);
    }
  }

  float3 envThroughput = path.throughput;

  // An escape from inside a medium has no exit distance, so the authored thickness stands in for it.
  if(path.mediumActive != 0)
  {
    envThroughput *= EvaluateMediumTransmittance(path.mediumAttenuation, path.mediumThickness);
  }

  PATH_TRACING_ON_CONTRIBUTION_SITE(path, kPathContributionSiteEscape);

  AccumulatePathContribution(path, envThroughput * envColor * misWeight);

  // The environment is infinitely far, so an escaping first bounce reports the far-hit distance.
  StoreFirstBounceHitDistance(path, kDenoiserFarHitDistance);
}

// Shades one path vertex: gathers its emission and next event estimation, then samples the bounce that continues the path.
// ray enters as the segment that found the vertex and, when this returns true, leaves as the bounce ray to trace next. False ends the path.
bool ShadePathVertex(inout PathState path, PathHitRecord hit, inout RayDesc ray, GltfSceneInfo sceneInfo)
{
  const SurfaceData surface = LoadSurfaceDataFromHit(hit, ray.Direction);
  const float3      viewDir = -ray.Direction;

  // Captured before the bounce increments path.depth, so it names this vertex.
  const uint vertexDepth = path.depth;

  PATH_TRACING_ON_SURFACE(path, surface, hit, sceneInfo);

  StorePrimarySurfaceForDenoiser(path, surface);
  StoreFirstBounceHitDistance(path, hit.hitDistance);

  ApplyCurrentMediumAttenuation(path, hit.hitDistance);

  // Emission
  // Emission found by a BSDF-sampled ray is MIS-weighted against light sampling of the same triangle.

  float emissiveMisWeight = 1.0;

  // Only when the previous vertex took an emissive light sample; otherwise nothing makes up the weight taken away here. PTShift's replay applies the same test.
  if(path.hasLastBsdfSample != 0 && path.lastEmissiveNeeActive != 0 && SafeMax3(surface.emission) > 0.0)
  {
    const float lightPdf = EvaluateCurrentEmissiveHitPdf(sceneInfo, hit.instanceIndex, hit.primitiveIndex, path.lastSurfacePosition, surface.worldPosition);

    if(lightPdf > 0.0 && path.lastDirectLightBsdfPdf > 0.0)
    {
      emissiveMisWeight = MisMixWeight(path.lastDirectLightBsdfPdf, lightPdf);
    }
  }

  PATH_TRACING_ON_CONTRIBUTION_SITE(path, kPathContributionSiteEmissionHit);

  AccumulatePathContribution(path, path.throughput * surface.emission * emissiveMisWeight);

  // Next event estimation
  // Each estimator draws from this vertex's own stream, so however many samples it consumes cannot shift the BSDF sample below, which replay must reproduce exactly.
  // At the primary hit this is the direct lighting a separate DI pass would otherwise own; ReSTIR PT resamples it as part of the same path tree (Section 6.1).

  path.seed = MakePathSampleStream(path.seed.pixel, path.seed.frame, vertexDepth, kPTStreamNee);

  PATH_TRACING_ON_CONTRIBUTION_SITE(path, kPathContributionSiteEnvironmentNee);

  AccumulateEnvironmentDirectLight(path, surface, sceneInfo, viewDir);

  path.seed = MakePathSampleStream(path.seed.pixel, path.seed.frame, vertexDepth, kPTStreamEmissive);

  PATH_TRACING_ON_CONTRIBUTION_SITE(path, kPathContributionSiteEmissiveNee);

  PATH_TRACING_ACCUMULATE_EMISSIVE_NEE(path, surface, hit, sceneInfo, viewDir, vertexDepth);

  if(path.depth >= pushConst.maxBounces)
  {
    return false;
  }

  // BSDF sample
  // Replay regenerates these exact lobe and direction dimensions. Roulette has its own stream because replay accounts for its density without redrawing survival.

  path.seed = MakePathSampleStream(path.seed.pixel, path.seed.frame, vertexDepth, kPTStreamBsdf);

  float3 bounceDir;
  float3 bsdfOverPdf         = (float3)0.0;
  float  directLightBsdfPdf  = 0.0;
  bool   isTransmissionEvent = false;
  uint   sampledLobeKind     = 0u;

  if(!SampleSurfaceBsdf(surface, viewDir, path.seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
  {
    return false;
  }

  const float geometricDot = dot(surface.geometricNormal, bounceDir);

  if(abs(geometricDot) <= 1.0e-6)
  {
    return false;
  }

  float3 nextThroughput = path.throughput * max(bsdfOverPdf, (float3)0.0);

  if(SafeMax3(nextThroughput) <= 0.0)
  {
    return false;
  }

  if(path.depth == 0u)
  {
    BeginFirstBounceDenoiserSignal(path, sampledLobeKind);
  }

  path.depth += 1;

  // Russian roulette, dividing survivors by their continuation probability so the estimate stays unbiased.
  if(PATH_TRACING_ROULETTE_ACTIVE(path))
  {
    const float continueProbability = clamp(SafeMax3(nextThroughput), 0.05, 0.95);

    path.seed = MakePathSampleStream(path.seed.pixel, path.seed.frame, vertexDepth, kPTStreamRoulette);

    if(NextRandom(path.seed) > continueProbability)
    {
      return false;
    }

    nextThroughput /= continueProbability;
  }

  PATH_TRACING_ON_BSDF_SAMPLE(path, surface, hit, viewDir, bounceDir, sampledLobeKind, vertexDepth, nextThroughput, sceneInfo);

  // Continue the path
  // Update the medium, record what the next vertex's MIS needs, and aim the bounce ray.

  path.throughput = nextThroughput;

  if(isTransmissionEvent)
  {
    if(surface.isFrontFace != 0 && HasVolumeAttenuation(surface))
    {
      path.mediumAttenuation = BuildAttenuationCoefficient(surface.attenuationColor, surface.attenuationDistance);
      path.mediumThickness   = surface.volumeThickness;
      path.mediumActive      = 1;
    }
    else if(surface.isFrontFace == 0)
    {
      path.mediumAttenuation = (float3)0.0;
      path.mediumThickness   = 0.0;
      path.mediumActive      = 0;
    }
  }

  path.lastSurfacePosition    = surface.worldPosition;
  path.lastSurfaceNormal      = surface.shadingNormal;
  path.lastDirectLightBsdfPdf = directLightBsdfPdf;

  bool environmentNeeActive = false;

  if(isTransmissionEvent)
  {
    environmentNeeActive = CanSampleEnvironmentTransmissionExitLight(surface) && directLightBsdfPdf > 0.0;
  }
  else
  {
    environmentNeeActive = CanSampleEnvironmentReflectionDirectLight(surface) && directLightBsdfPdf > 0.0;
  }

  path.lastEnvironmentNeeActive = environmentNeeActive ? 1u : 0u;
  path.lastEmissiveNeeActive    = CanSampleEmissiveDirectLight(surface, sceneInfo) ? 1u : 0u;
  path.hasLastBsdfSample        = 1u;

  const float3 originNormal = SelectOffsetNormal(surface.geometricNormal, bounceDir);

  ray.Origin    = OffsetRay(surface.worldPosition, originNormal);
  ray.Direction = bounceDir;
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  return true;
}

// Traces a whole path from its camera ray. It ends when a segment escapes or a vertex stops the path, which happens at the latest at pushConst.maxBounces.
void TracePath(inout PathState path, RayDesc cameraRay, GltfSceneInfo sceneInfo)
{
  RayDesc ray = cameraRay;

  while(true)
  {
    const PathHitRecord hit = TracePathSegment(ray);

    if(hit.hasHit == 0u)
    {
      AccumulatePathEscape(path, ray.Direction, sceneInfo);
      return;
    }

    if(!ShadePathVertex(path, hit, ray, sceneInfo))
    {
      return;
    }
  }
}

#endif
