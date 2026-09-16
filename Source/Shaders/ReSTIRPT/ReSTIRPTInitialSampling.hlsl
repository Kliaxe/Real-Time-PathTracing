// Initial sampling
// ReSTIR PT Enhanced. Generates one path tree per pixel and resamples it down to a single representative path (paper Section 2.3, with direct light unified into the same tree per Section 6.1).
// The path tracing itself is the shared path loop (PathLoop.hlsli) the reference path tracer runs, customized only through its hook macros. The central difference is the contribution hook: where the reference tracer sums contributions into radiance, this pass additionally streams each one through reservoir sampling.
// Because the hooks only observe, path.radiance still holds the exact reference answer for this sample, which the debug flag can output directly for an in-pass comparison against the resampled estimate.

#include <Common/ShaderTypes.h>
#include "ShaderIncludes/Random.hlsli"
#include "ShaderIncludes/Sky.hlsli"
#include "ShaderIncludes/Camera.hlsli"
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PTGlobals.hlsli"
#include "ShaderIncludes/ReSTIR/Common.hlsli"
#include "ReSTIR/PTReservoir.hlsli"

// Declared ahead of the hook macros below, which call it.
float NextRandom(inout uint seed);

// Contribution hooks
// These macros customize the shared path tracing headers included after them. The path loop hooks are defined further down, after the shared types they need.

// The reservoir travels with the path state so every contribution site can reach it.
// ptResamplingSeed is deliberately separate from path.seed: consuming the path tracer's RNG stream for reservoir selection would shift the sampling sequence and break bit-exact parity with the reference tracer for the same pixel/frame.
#define PATH_TRACING_STATE_EXTRA_FIELDS \
  ReSTIRPTReservoir          ptReservoir; \
  ReSTIRPTReconnectionSearch ptRcSearch;  \
  uint              ptResamplingSeed;     \
  float3            ptPrimaryGeometricNormal;

// Every radiance contribution the tree produces becomes one resampling candidate with a unit candidate weight.
#define PATH_TRACING_ON_CONTRIBUTION(path, contribution)                                                 \
  PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(path, contribution, 1.0)

// A contribution that arrives with its own unbiased contribution weight; RIS-based NEE is the only such site today. The weight belongs in the resampling weight, not in the stored integrand, which is why it is threaded through rather than folded in at the call site.
// NextRandom is drawn here rather than inside the reservoir so the resampling stream stays visibly separate from the path tracer's own.
// The reconnection vertex is attached at the moment of selection, not at the end: the search state describes the prefix of the path as it stands right now, and by the time the tree finishes it describes a different path.
#define PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(path, contribution, candidateWeight)                       \
  if(StreamPTCandidate(path.ptReservoir, contribution, candidateWeight,                                 \
                       NextRandom(path.ptResamplingSeed)))                                               \
  {                                                                                                      \
    ApplyPTReconnectionToReservoir(path.ptReservoir, path.ptRcSearch, path.depth);                       \
  }

// Records the emissive NEE sample so the contribution hook, firing immediately afterwards, can adopt the light vertex as this path's reconnection anchor.
#define PATH_TRACING_ON_NEE_EMISSIVE(path, lightIndex, barycentrics, radiance, lightPdf, distance)       \
  path.ptRcSearch.neeLightIndex   = lightIndex;                                                          \
  path.ptRcSearch.neeBarycentrics = float2(barycentrics.y, barycentrics.z);                              \
  path.ptRcSearch.neeRadiance     = radiance;                                                            \
  path.ptRcSearch.neePdf          = lightPdf;                                                            \
  path.ptRcSearch.neeShadingDepth = path.depth;                                                          \
  path.ptRcSearch.neeValid        = 1u;

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"
#include "ShaderIncludes/PathTracing/Volume.hlsli"
#include "ShaderIncludes/PathTracing/Hdri.hlsli"
#include "ShaderIncludes/PathTracing/Disney.hlsli"
#include "ShaderIncludes/PathTracing/Intersection.hlsli"
#include "ShaderIncludes/PathTracing/Lights.hlsli"
#include "ReSTIR/PTReservoirStorage.hlsli"
#include "ShaderIncludes/ReSTIR/PTNeeRis.hlsli"

ReSTIRPTSurface EmptyPTSurface()
{
  ReSTIRPTSurface surface;

  surface.worldPosition   = (float3)0.0;
  surface.linearDepth     = 0.0;
  surface.shadingNormal   = (float3)0.0;
  surface.roughness       = 0.0;
  surface.geometricNormal = (float3)0.0;
  surface.metallic        = 0.0;
  surface.albedo          = (float3)0.0;
  surface.valid           = 0;
  surface.instanceIndex   = 0;
  surface.primitiveIndex  = 0;
  surface.barycentrics    = (float2)0.0;

  return surface;
}

// Publishes the reduced primary-surface record later passes use for similarity tests and for rebuilding this surface, and keeps the one term the rest of this path still needs.
// It is written here, at the primary hit, rather than assembled from path state at the end: the hit identity, albedo and depth are in hand exactly once, and ferrying them to the end of ray generation cost eight dwords of per-path state that the whole loop then had to carry through every bounce.
// The geometric normal is the exception and stays in the state, because the reconnection criteria compare against it at every later vertex.
void PublishPTPrimarySurface(inout PathState path, SurfaceData surface, PathHitRecord hit, GltfSceneInfo sceneInfo)
{
  if(path.hasPrimarySurface != 0u)
  {
    return;
  }

  path.ptPrimaryGeometricNormal = surface.geometricNormal;

  ReSTIRPTSurface record;

  record.worldPosition   = surface.worldPosition;
  record.linearDepth     = length(surface.worldPosition - sceneInfo.cameraPosition);
  record.shadingNormal   = surface.shadingNormal;
  record.roughness       = surface.roughness;
  record.geometricNormal = surface.geometricNormal;
  record.metallic        = surface.metallic;
  record.albedo          = surface.albedo;
  record.valid           = 1;
  record.instanceIndex   = hit.instanceIndex;
  record.primitiveIndex  = hit.primitiveIndex;
  record.barycentrics    = hit.barycentrics;

  const uint2 launchID = DispatchRaysIndex().xy;

  currentSurfaceBuffer[launchID.y * uint(sceneInfo.viewportSize.x) + launchID.x] = record;
}

// The endpoint kind a candidate from each contribution site records if it is selected, so replay knows which estimator produced the path.
// An escape is recorded as an environment-miss endpoint, which replay knows to let miss.
uint PTEndpointKindForSite(uint site)
{
  if(site == kPathContributionSiteEmissionHit)
  {
    return RESTIR_PT_ENDPOINT_KIND_BSDF_EMISSIVE;
  }

  if(site == kPathContributionSiteEnvironmentNee)
  {
    return RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT;
  }

  if(site == kPathContributionSiteEmissiveNee)
  {
    return RESTIR_PT_ENDPOINT_KIND_EMISSIVE_NEE;
  }

  return RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT_MISS;
}

// Emissive NEE at one vertex, with RIS over the light tiles when they are enabled (Section 6.1).
// The RIS variant consumes a different number of random values than the single-sample one, which is safe only because the BSDF sample is re-seeded from its own per-vertex stream rather than continuing this one. Replay never re-executes NEE at an emissive endpoint (that endpoint carries a forced anchor and is shifted geometrically), so the two variants stay interchangeable from the shift's point of view.
void AccumulatePTEmissiveNee(inout PathState path, SurfaceData surface, PathHitRecord hit, GltfSceneInfo sceneInfo, float3 viewDir, uint vertexDepth)
{
  if(ptParams.nee.enableLightTiles != 0u)
  {
    AccumulatePTEmissiveDirectLightRis(path, surface, hit.instanceIndex, hit.primitiveIndex, sceneInfo, viewDir, DispatchRaysIndex().xy, vertexDepth);
    return;
  }

  AccumulateEmissiveDirectLight(path, surface, hit.instanceIndex, hit.primitiveIndex, sceneInfo, viewDir);
}

// Reconnection vertex search
// Paper Section 2.3. The first vertex whose pair with its predecessor passes the criteria wins and is never revisited: fixing the reconnection point early is what bounds the replay length and keeps the shift's cost predictable.
// Runs for every bounce that survived roulette, while path still describes the predecessor, and then records this vertex as the predecessor of the next.
void UpdatePTReconnectionSearch(inout PathState path, SurfaceData surface, PathHitRecord hit, float3 viewDir, float3 bounceDir, uint sampledLobeKind, uint vertexDepth, float3 nextThroughput, GltfSceneInfo sceneInfo)
{
  // Leaving density
  // The density of the group the sampler chose, recovered by re-evaluating it for the sampled direction. The reconnection criteria and the Jacobian's base denominator below need it as this vertex's outgoing density, and the next vertex needs it as its predecessor's, so it is evaluated once for both.

  float vertexSamplePdf = 0.0;

  EvaluateSurfaceBsdfGroup(surface, viewDir, bounceDir, sampledLobeKind, vertexSamplePdf);

  // Candidate test
  // Requires vertexDepth >= 1 because the criteria compare two surfaces, and the predecessor of the primary hit is the camera, which has no roughness.

  if(path.ptRcSearch.found == 0 && vertexDepth >= 1)
  {
    const float connectionDistance = length(surface.worldPosition - path.lastSurfacePosition);

    bool qualifies;

    if(ptParams.shift.reconnectionCriteria == RESTIR_PT_RECONNECTION_CRITERIA_FOOTPRINT)
    {
      // Section 4.2's single-vertex roughness threshold is retained alongside the footprint test, guarding cases the footprint bound cannot cover: parallax, curvature, and reconnection to lights at infinity.
      const bool passesRoughnessGuard = path.ptRcSearch.prevRoughness >= ptParams.shift.minRoughness;

      // Footnote 6: a broad-group (diffuse-like) or emissive reconnection vertex does not change its outgoing density under reconnection, so the inverse half is skipped.
      const bool skipInverseFootprint = (sampledLobeKind == 0u) || SafeMax3(surface.emission) > 0.0;

      qualifies = passesRoughnessGuard && PassesFootprintReconnectionCriteria(path.ptRcSearch.prevSamplePdf, vertexSamplePdf, path.lastSurfacePosition, path.ptRcSearch.prevGeometricNormal, surface.worldPosition, surface.geometricNormal, path.primaryWorldPosition, path.ptPrimaryGeometricNormal, sceneInfo.cameraPosition, ptParams.shift.footprintThreshold, skipInverseFootprint);
    }
    else
    {
      qualifies = PassesLegacyReconnectionCriteria(path.ptRcSearch.prevRoughness, SurfaceBsdfGroupRoughness(surface, sampledLobeKind), connectionDistance, ptParams.shift.minRoughness, ptParams.shift.legacyMinDistance);
    }

    if(qualifies)
    {
      path.ptRcSearch.found          = 1;
      path.ptRcSearch.length         = vertexDepth;
      path.ptRcSearch.instanceId     = hit.instanceIndex;
      path.ptRcSearch.primitiveIndex = hit.primitiveIndex;
      path.ptRcSearch.barycentrics   = hit.barycentrics;
      path.ptRcSearch.wi             = bounceDir;
      path.ptRcSearch.lobeKind       = sampledLobeKind;

      // Frozen here, not read at contribution time: every later vertex overwrites the running prev* fields.
      path.ptRcSearch.rcPrevLobeKind = path.ptRcSearch.prevLobeKind;

      // Post-roulette throughput, so dividing a later integrand by it recovers exactly the suffix radiance the shift will reuse.
      path.ptRcSearch.suffixThroughput = nextThroughput;

      // Base Jacobian denominator
      // Equation 2's D_x is assembled while both vertices are still in hand. The density at this vertex is the vertexSamplePdf recovered above by re-evaluating the group the sampler chose, which reproduces the very pdf it divided by.
      // The geometry term is single-sided, measured at the source vertex, matching the paper's definition. For a self-shift the convention cancels; it only starts to matter once source and destination pixels differ.

      const float3 fromPrevious   = surface.worldPosition - path.lastSurfacePosition;
      const float  previousDistSq = max(dot(fromPrevious, fromPrevious), 1.0e-8);
      const float3 previousDir    = fromPrevious * rsqrt(previousDistSq);
      const float  geometryTerm   = max(0.0, dot(path.ptRcSearch.prevGeometricNormal, previousDir)) / previousDistSq;

      path.ptRcSearch.jacobianTerms = path.ptRcSearch.prevSamplePdf * geometryTerm * vertexSamplePdf;
    }
  }

  // Predecessor state
  // Tracked for the next vertex, which needs this vertex's criteria input and the density/lobe of the event leaving it. The shared state already carries the predecessor position, but none of the rest.

  // Section 7.5's lobe-specific connectability: the roughness that matters at this vertex is the one of the group that sampled the path's way out of it, not the material's aggregate.
  path.ptRcSearch.prevRoughness       = SurfaceBsdfGroupRoughness(surface, sampledLobeKind);
  path.ptRcSearch.prevLobeKind        = sampledLobeKind;
  path.ptRcSearch.prevGeometricNormal = surface.geometricNormal;
  path.ptRcSearch.prevSamplePdf       = vertexSamplePdf;
}

// Path loop hooks
// ReSTIR PT's additions to the shared loop, each forwarding to a function above.

#define PATH_TRACING_ON_SURFACE(path, surface, hit, sceneInfo) PublishPTPrimarySurface(path, surface, hit, sceneInfo)

// Tags the site before it publishes: the selection hook fires inside the accumulation and has no other way to know which estimator produced the candidate.
#define PATH_TRACING_ON_CONTRIBUTION_SITE(path, site) path.ptRcSearch.pendingEndpointKind = PTEndpointKindForSite(site)

#define PATH_TRACING_ACCUMULATE_EMISSIVE_NEE(path, surface, hit, sceneInfo, viewDir, vertexDepth) AccumulatePTEmissiveNee(path, surface, hit, sceneInfo, viewDir, vertexDepth)

// Russian roulette (Section 6.2.4). Applied only here, during initial sampling. It must never run during random replay, because killing a path the base path survived makes the shift fail for reasons unrelated to the surfaces involved.
#define PATH_TRACING_ROULETTE_ACTIVE(path) (ptParams.initialSampling.enableRussianRoulette != 0u && (path).depth >= ptParams.initialSampling.russianRouletteStartBounce)

#define PATH_TRACING_ON_BSDF_SAMPLE(path, surface, hit, viewDir, bounceDir, sampledLobeKind, vertexDepth, nextThroughput, sceneInfo) UpdatePTReconnectionSearch(path, surface, hit, viewDir, bounceDir, sampledLobeKind, vertexDepth, nextThroughput, sceneInfo)

#include "ShaderIncludes/PathTracing/PathLoop.hlsli"
#include "ShaderIncludes/PathTracing/PathRayEntryPoints.hlsli"

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID   = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo  = pushConst.sceneInfoAddress.Get();
  const uint          width      = uint(sceneInfo.viewportSize.x);
  const uint          pixelIndex = launchID.y * width + launchID.x;

  // Path state
  // The shared fields start exactly as the reference tracer's do, so the two trace the same path for the same pixel and frame. The ReSTIR fields start with an empty reservoir that remembers the sampling origin replay will regenerate the path from.

  PathState path = BeginPath(launchID, sceneInfo);

  path.ptReservoir                = EmptyPTReservoir();
  path.ptReservoir.initRandomSeed = XxHash32(uint3(launchID, pushConst.rngFrameNumber));
  path.ptReservoir.samplePixel    = PackPathSamplePixel(launchID);
  path.ptReservoir.sampleFrame    = pushConst.rngFrameNumber;
  path.ptRcSearch                 = EmptyPTReconnectionSearch();

  // Offsetting the frame number decorrelates reservoir selection from path sampling; they must not share a stream.
  path.ptResamplingSeed = XxHash32(uint3(launchID, pushConst.rngFrameNumber + 0x9E3779B9u));

  path.ptPrimaryGeometricNormal = (float3)0.0;

  // Surface record
  // Written empty before the path is traced and overwritten at the primary hit, so a camera ray that escapes leaves behind the invalid record later passes expect without the loop having to report its miss back here.

  currentSurfaceBuffer[pixelIndex] = EmptyPTSurface();

  TracePath(path, MakeCameraRay(launchID, sceneInfo), sceneInfo);

  // One path tree is one sample of path space regardless of how many candidates it contained, so the confidence weight is 1 rather than the candidate count.
  FinalizePTReservoir(path.ptReservoir, 1.0);

  // Correctness gate. Replacing the resampled sample with the reference sum, with ucw set to 1, sends the plain path-traced answer down the exact same storage, resolve, and accumulation path as the resampled estimate. Toggling the flag therefore isolates the resampling step and nothing else: with reuse disabled the two must converge to the same image.
  if((pushConst.flags & uint(ReSTIRPTFlags::eReSTIRPTFlagReferenceRadiance)) != 0u)
  {
    path.ptReservoir.F         = max(path.radiance, (float3)0.0);
    path.ptReservoir.ucw       = 1.0;
    path.ptReservoir.targetPdf = ReSTIRLuminance(path.ptReservoir.F);
    path.ptReservoir.M         = 1.0;
  }

  // Store
  // Section 6.3. With no reuse yet there is one candidate, so the vector weight is just its contribution; writing it here keeps final shading uniform across resampling modes instead of special-casing "no reuse ran".

  ptShadingWeightBuffer[pixelIndex] = path.ptReservoir.F * path.ptReservoir.ucw;

  StorePTReservoir(path.ptReservoir, ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(launchID), ptParams.bufferIndices.initialSamplingOutputBufferIndex);

  // Denoiser guides
  // Hands final shading what only this pass can know about the path it traced.
  // A first bounce that was sampled but escaped the scene still reports a distance: the transport is real and merely far away. A lobe that was never sampled reports zero, which is what NRD defines as "no hit distance for this signal" and repairs from neighbouring pixels (see hitDistanceReconstructionMode in ReSTIRPTSettings).
  // The two cases must not be conflated: pretending an unsampled lobe was a distant hit would feed the denoiser a fabricated blur radius.

  if(path.awaitingFirstBounceHitDistance != 0u)
  {
    StoreFirstBounceHitDistance(path, kDenoiserFarHitDistance);
  }

  // Specular energy share
  // How much of this path's energy arrived through the specular lobe. An energy ratio rather than the lobe flag: the flag is one Bernoulli draw, whereas every contribution the tree collected (each NEE event, each bounce) lands in one of these two sums, so the ratio moves continuously with what the path actually carried.
  // Negative marks a path that collected nothing, where the ratio has no meaning and final shading falls back to the surface's own material response.

  const float diffuseLuminance  = ReSTIRLuminance(max(path.diffuseDenoiserRadiance, (float3)0.0));
  const float specularLuminance = ReSTIRLuminance(max(path.specularDenoiserRadiance, (float3)0.0));
  const float totalLuminance    = diffuseLuminance + specularLuminance;

  ptDenoiserGuideBuffer[pixelIndex] = float3(path.hasDiffuseDenoiserHitDistance != 0u ? path.diffuseDenoiserHitDistance : 0.0, path.hasSpecularDenoiserHitDistance != 0u ? path.specularDenoiserHitDistance : 0.0, totalLuminance > 0.0 ? (specularLuminance / totalLuminance) : -1.0);
}
