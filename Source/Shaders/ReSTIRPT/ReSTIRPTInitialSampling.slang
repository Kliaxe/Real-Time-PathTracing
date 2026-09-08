/*
 * ReSTIR PT Enhanced - initial sampling.
 *
 * Generates one path tree per pixel and resamples it down to a single
 * representative path (paper Section 2.3, with direct light unified into the same
 * tree per Section 6.1).
 *
 * The path tracing itself is the shared code used by the reference path tracer,
 * reused verbatim through PATH_TRACING_CUSTOM_GLOBALS. The only behavioral
 * difference is the contribution hook: where the reference tracer sums
 * contributions into radiance, this pass additionally streams each one through
 * reservoir sampling. Because the hook only observes, payload.radiance still holds
 * the exact reference answer for this sample, which the debug flag can output
 * directly for an in-pass comparison against the resampled estimate.
 */

#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "nvshaders/sky_functions.h.slang"
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PTGlobals.h.slang"
#include "ShaderIncludes/ReSTIR/Common.h.slang"
#include "ReSTIR/PTReservoir.hlsli"

// The reservoir travels with the path so every contribution site can reach it.
// ptResamplingSeed is deliberately separate from payload.seed: consuming the path
// tracer's RNG stream for reservoir selection would shift the sampling sequence
// and break bit-exact parity with the reference tracer for the same pixel/frame.
#define PATH_TRACING_PAYLOAD_EXTRA_FIELDS \
  ReSTIRPTReservoir          ptReservoir; \
  ReSTIRPTReconnectionSearch ptRcSearch;  \
  uint              ptResamplingSeed;     \
  float3            ptPrimaryGeometricNormal; \
  float3            ptPrimaryAlbedo;      \
  float             ptPrimaryLinearDepth;

// Every radiance contribution the tree produces becomes one resampling candidate.
// NextRandom is drawn here rather than inside the reservoir so the resampling
// stream stays visibly separate from the path tracer's own.
//
// The reconnection vertex is attached at the moment of selection, not at the end:
// the search state describes the prefix of the path as it stands right now, and by
// the time the tree finishes it describes a different path.
#define PATH_TRACING_ON_CONTRIBUTION(payload, contribution)                                              \
  PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(payload, contribution, 1.0)

// A contribution that arrives with its own unbiased contribution weight - RIS-based
// NEE is the only such site today. The weight belongs in the resampling weight, not
// in the stored integrand, which is why it is threaded through rather than folded
// in at the call site.
#define PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(payload, contribution, candidateWeight)                    \
  if(StreamPTCandidate(payload.ptReservoir, contribution, candidateWeight,                              \
                       NextRandom(payload.ptResamplingSeed)))                                            \
  {                                                                                                      \
    ApplyPTReconnectionToReservoir(payload.ptReservoir, payload.ptRcSearch, payload.depth);              \
  }

// Records the emissive NEE sample so the contribution hook, firing immediately
// afterwards, can adopt the light vertex as this path's reconnection anchor.
#define PATH_TRACING_ON_NEE_EMISSIVE(payload, lightIndex, barycentrics, radiance, lightPdf, distance)    \
  payload.ptRcSearch.neeLightIndex   = lightIndex;                                                       \
  payload.ptRcSearch.neeBarycentrics = float2(barycentrics.y, barycentrics.z);                           \
  payload.ptRcSearch.neeRadiance     = radiance;                                                         \
  payload.ptRcSearch.neePdf          = lightPdf;                                                         \
  payload.ptRcSearch.neeShadingDepth = payload.depth;                                                    \
  payload.ptRcSearch.neeValid        = 1u;

#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"
#include "ReSTIR/PTReservoirStorage.hlsli"
#include "ShaderIncludes/ReSTIR/PTNeeRis.h.slang"

// Purpose tags selecting one of the per-vertex random streams.
static const uint kPTStreamNee  = 0u;
static const uint kPTStreamBsdf = 1u;

// Indexed random stream for one vertex of one path.
//
// The path tracer normally advances a single running seed, so the random numbers
// a vertex receives depend on how many draws every earlier decision happened to
// consume. That is fatal for random replay: reproducing a path's BSDF directions
// from a different pixel would require reproducing every NEE branch that came
// before, including data-dependent early-outs.
//
// Deriving an independent stream per (vertex, purpose) makes each vertex's BSDF
// sample addressable on its own, which is what the paper's primary sample space
// parameterization assumes. Replay can then regenerate vertex j's direction
// without executing - or even knowing about - next event estimation.
uint PTVertexSeed(uint initialSeed, uint vertexDepth, uint purpose)
{
  return xxhash32(uint3(initialSeed, vertexDepth, purpose));
}

ReSTIRPTSurface EmptyPTSurface()
{
  ReSTIRPTSurface surface;
  surface.worldPosition   = float3(0.0);
  surface.linearDepth     = 0.0;
  surface.shadingNormal   = float3(0.0);
  surface.roughness       = 0.0;
  surface.geometricNormal = float3(0.0);
  surface.metallic        = 0.0;
  surface.albedo          = float3(0.0);
  surface.valid           = 0;
  return surface;
}

ReSTIRPTSurface BuildPTSurfaceFromPayload(PathPayload payload)
{
  ReSTIRPTSurface surface = EmptyPTSurface();
  if(payload.hasPrimarySurface == 0u)
  {
    return surface;
  }

  surface.worldPosition   = payload.primaryWorldPosition;
  surface.linearDepth     = payload.ptPrimaryLinearDepth;
  surface.shadingNormal   = payload.primaryShadingNormal;
  surface.roughness       = payload.primaryRoughness;
  surface.geometricNormal = payload.ptPrimaryGeometricNormal;
  surface.metallic        = payload.primaryMetalness;
  surface.albedo          = payload.ptPrimaryAlbedo;
  surface.valid           = 1;
  return surface;
}

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID  = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const uint          width     = uint(sceneInfo.viewportSize.x);
  const uint          pixelIndex = launchID.y * width + launchID.x;

  RayDesc ray;
  ray.Origin    = sceneInfo.cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel(float2(launchID) + 0.5, sceneInfo);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  // Seeding matches the reference tracer exactly so the two produce the same path
  // for the same pixel and frame.
  const uint initialSeed = xxhash32(uint3(launchID, pushConst.rngFrameNumber));

  PathPayload payload;
  payload.radiance                 = float3(0.0);
  payload.throughput               = float3(1.0);
  payload.mediumAttenuation        = float3(0.0);
  payload.mediumThickness          = 0.0;
  payload.lastSurfacePosition      = sceneInfo.cameraPosition;
  payload.lastSurfaceNormal        = float3(0.0, 1.0, 0.0);
  payload.lastDirectLightBsdfPdf   = 0.0;
  payload.seed                     = initialSeed;
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

  payload.ptReservoir              = EmptyPTReservoir();
  payload.ptReservoir.initRandomSeed = initialSeed;
  payload.ptRcSearch               = EmptyPTReconnectionSearch();
  // Offsetting the frame number decorrelates reservoir selection from path
  // sampling; they must not share a stream.
  payload.ptResamplingSeed         = xxhash32(uint3(launchID, pushConst.rngFrameNumber + 0x9E3779B9u));
  payload.ptPrimaryGeometricNormal = float3(0.0);
  payload.ptPrimaryAlbedo          = float3(0.0);
  payload.ptPrimaryLinearDepth     = 0.0;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, payload);

  // One path tree is one sample of path space regardless of how many candidates it
  // contained, so the confidence weight is 1 rather than the candidate count.
  FinalizePTReservoir(payload.ptReservoir, 1.0);

  if((pushConst.flags & uint(ReSTIRPTFlags::eReSTIRPTFlagReferenceRadiance)) != 0u)
  {
    // Correctness gate. Replacing the resampled sample with the reference sum,
    // while leaving ucw at 1, sends the plain path-traced answer down the exact
    // same storage, resolve, and accumulation path as the resampled estimate.
    // Toggling the flag therefore isolates the resampling step and nothing else:
    // with reuse disabled the two must converge to the same image.
    payload.ptReservoir.F         = max(payload.radiance, float3(0.0));
    payload.ptReservoir.ucw       = 1.0;
    payload.ptReservoir.targetPdf = ReSTIRLuminance(payload.ptReservoir.F);
    payload.ptReservoir.M         = 1.0;
  }

  currentSurfaceBuffer[pixelIndex] = BuildPTSurfaceFromPayload(payload);
  // Section 6.3. With no reuse yet there is one candidate, so the vector weight is
  // just its contribution; writing it here keeps final shading uniform across
  // resampling modes instead of special-casing "no reuse ran".
  ptShadingWeightBuffer[pixelIndex] = payload.ptReservoir.F * payload.ptReservoir.ucw;

  StorePTReservoir(payload.ptReservoir, ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(launchID),
                   ptParams.bufferIndices.initialSamplingOutputBufferIndex);

  // Hand final shading what only this pass can know about the path it traced.
  //
  // A first bounce that was sampled but escaped the scene still reports a distance:
  // the transport is real and merely far away. A lobe that was never sampled reports
  // zero, which is what NRD defines as "no hit distance for this signal" and repairs
  // from neighbouring pixels - see hitDistanceReconstructionMode in ReSTIRPTSettings.
  // The two cases must not be conflated: pretending an unsampled lobe was a distant
  // hit would feed the denoiser a fabricated blur radius.
  if(payload.awaitingFirstBounceHitDistance != 0u)
  {
    StoreFirstBounceHitDistance(payload, kDenoiserFarHitDistance);
  }

  // How much of this path's energy arrived through the specular lobe. An energy
  // ratio rather than the lobe flag: the flag is one Bernoulli draw, whereas every
  // contribution the tree collected - each NEE event, each bounce - lands in one of
  // these two sums, so the ratio moves continuously with what the path actually
  // carried. Negative marks a path that collected nothing, where the ratio has no
  // meaning and final shading falls back to the surface's own material response.
  const float diffuseLuminance  = ReSTIRLuminance(max(payload.diffuseDenoiserRadiance, float3(0.0)));
  const float specularLuminance = ReSTIRLuminance(max(payload.specularDenoiserRadiance, float3(0.0)));
  const float totalLuminance    = diffuseLuminance + specularLuminance;

  ptDenoiserGuideBuffer[pixelIndex] =
      float3(payload.hasDiffuseDenoiserHitDistance != 0u ? payload.diffuseDenoiserHitDistance : 0.0,
             payload.hasSpecularDenoiserHitDistance != 0u ? payload.specularDenoiserHitDistance : 0.0,
             totalLuminance > 0.0 ? (specularLuminance / totalLuminance) : -1.0);
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

  payload.ptRcSearch.pendingEndpointKind = RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT_MISS;
  AccumulatePathContribution(payload, envThroughput * envColor * misWeight);
  StoreFirstBounceHitDistance(payload, kDenoiserFarHitDistance);
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
  // Captured before the bounce increments it, so it names this vertex.
  const uint          vertexDepth = payload.depth;

  // Capture the extra primary-surface terms the shared helper does not carry.
  // Later passes need geometry and albedo for surface similarity and shift tests.
  if(payload.hasPrimarySurface == 0u)
  {
    payload.ptPrimaryGeometricNormal = surface.geometricNormal;
    payload.ptPrimaryAlbedo          = surface.albedo;
    payload.ptPrimaryLinearDepth     = length(surface.worldPosition - sceneInfo.cameraPosition);
  }

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

  // Tag the site before it publishes: the selection hook fires inside these calls
  // and has no other way to know which estimator produced the candidate.
  payload.ptRcSearch.pendingEndpointKind = RESTIR_PT_ENDPOINT_KIND_BSDF_EMISSIVE;
  AccumulatePathContribution(payload, payload.throughput * surface.emission * emissiveMisWeight);

  // Next event estimation. At the primary hit this is the direct lighting that a
  // separate ReSTIR DI pass would otherwise own; here it simply joins the same
  // path tree as a length-2 candidate (Section 6.1).
  //
  // NEE draws from this vertex's own stream so its consumption cannot shift the
  // BSDF sample below, which replay must reproduce exactly.
  payload.seed = PTVertexSeed(payload.ptReservoir.initRandomSeed, vertexDepth, kPTStreamNee);
  payload.ptRcSearch.pendingEndpointKind = RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT;
  AccumulateEnvironmentDirectLight(payload, surface, sceneInfo, viewDir);
  payload.ptRcSearch.pendingEndpointKind = RESTIR_PT_ENDPOINT_KIND_EMISSIVE_NEE;
  // Section 6.1. The RIS variant consumes a different number of random values than
  // the single-sample one, which is safe only because the BSDF sample below is
  // re-seeded from its own per-vertex stream rather than continuing this one.
  // Replay never re-executes NEE at an emissive endpoint - that endpoint carries a
  // forced anchor and is shifted geometrically - so the two variants stay
  // interchangeable from the shift's point of view.
  if(ptParams.nee.enableLightTiles != 0u)
  {
    AccumulatePTEmissiveDirectLightRis(payload, surface, sceneInfo, viewDir, DispatchRaysIndex().xy, vertexDepth);
  }
  else
  {
    AccumulateEmissiveDirectLight(payload, surface, sceneInfo, viewDir);
  }

  if(payload.depth >= pushConst.maxBounces)
  {
    return;
  }

  // The stream replay regenerates. Everything downstream of here at this vertex -
  // BSDF sampling and then Russian roulette - shares it, which is safe because
  // replay stops before roulette and never consumes past the direction sample.
  payload.seed = PTVertexSeed(payload.ptReservoir.initRandomSeed, vertexDepth, kPTStreamBsdf);

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

  // Russian roulette (Section 6.2.4). Applied only here, during initial sampling.
  // It must never run during random replay, because killing a path the base path
  // survived makes the shift fail for reasons unrelated to the surfaces involved.
  if(ptParams.initialSampling.enableRussianRoulette != 0u
     && payload.depth >= ptParams.initialSampling.russianRouletteStartBounce)
  {
    const float continueProbability = clamp(SafeMax3(nextThroughput), 0.05, 0.95);
    if(NextRandom(payload.seed) > continueProbability)
    {
      return;
    }

    nextThroughput /= continueProbability;
  }

  // Reconnection vertex search (paper Section 2.3). The first vertex whose pair
  // with its predecessor passes the criteria wins and is never revisited: fixing
  // the reconnection point early is what bounds the replay length and keeps the
  // shift's cost predictable.
  //
  // Requires vertexDepth >= 1 because the criteria compare two surfaces, and the
  // predecessor of the primary hit is the camera, which has no roughness.
  if(payload.ptRcSearch.found == 0 && vertexDepth >= 1)
  {
    const float connectionDistance = length(surface.worldPosition - payload.lastSurfacePosition);

    // Both criteria need this vertex's own outgoing density, which is recovered by
    // re-evaluating the group the sampler chose - the same value the Jacobian's
    // base denominator uses below.
    float vertexSamplePdf = 0.0;
    EvaluateSurfaceBsdfGroup(surface, viewDir, bounceDir, sampledLobeKind, vertexSamplePdf);

    bool qualifies;
    if(ptParams.shift.reconnectionCriteria == RESTIR_PT_RECONNECTION_CRITERIA_FOOTPRINT)
    {
      // Section 4.2's single-vertex roughness threshold is retained alongside the
      // footprint test, guarding cases the footprint bound cannot cover: parallax,
      // curvature, and reconnection to lights at infinity.
      const bool passesRoughnessGuard = payload.ptRcSearch.prevRoughness >= ptParams.shift.minRoughness;
      // Footnote 6: a broad-group (diffuse-like) reconnection vertex does not change
      // its outgoing density under reconnection, so the inverse half is skipped.
      const bool skipInverseFootprint = (sampledLobeKind == 0u) || SafeMax3(surface.emission) > 0.0;

      qualifies = passesRoughnessGuard
                  && PassesFootprintReconnectionCriteria(payload.ptRcSearch.prevSamplePdf, vertexSamplePdf,
                                                         payload.lastSurfacePosition, payload.ptRcSearch.prevGeometricNormal,
                                                         surface.worldPosition, surface.geometricNormal,
                                                         payload.primaryWorldPosition, payload.ptPrimaryGeometricNormal,
                                                         sceneInfo.cameraPosition, ptParams.shift.footprintThreshold,
                                                         skipInverseFootprint);
    }
    else
    {
      qualifies = PassesLegacyReconnectionCriteria(payload.ptRcSearch.prevRoughness, surface.roughness, connectionDistance,
                                                  ptParams.shift.minRoughness, ptParams.shift.legacyMinDistance);
    }
    if(qualifies)
    {
      payload.ptRcSearch.found          = 1;
      payload.ptRcSearch.length         = vertexDepth;
      payload.ptRcSearch.instanceId     = InstanceIndex();
      payload.ptRcSearch.primitiveIndex = PrimitiveIndex();
      payload.ptRcSearch.barycentrics   = attr.barycentrics;
      payload.ptRcSearch.wi             = bounceDir;
      payload.ptRcSearch.lobeKind       = sampledLobeKind;
      // Frozen here, not read at contribution time: every later vertex overwrites
      // the running prev* fields.
      payload.ptRcSearch.rcPrevLobeKind = payload.ptRcSearch.prevLobeKind;
      // Post-roulette throughput, so dividing a later integrand by it recovers
      // exactly the suffix radiance the shift will reuse.
      payload.ptRcSearch.suffixThroughput = nextThroughput;

      // Assemble Equation 2's base denominator while both vertices are still in
      // hand. The density at this vertex is recovered by re-evaluating the group
      // the sampler chose, which reproduces the very pdf it divided by; asking
      // SampleSurfaceBsdf for it is not possible because it only exports the

      const float3 fromPrevious   = surface.worldPosition - payload.lastSurfacePosition;
      const float  previousDistSq = max(dot(fromPrevious, fromPrevious), 1.0e-8);
      const float3 previousDir    = fromPrevious * rsqrt(previousDistSq);
      // Single-sided geometry term measured at the source vertex, matching the
      // paper's definition. For a self-shift the convention cancels; it only
      // starts to matter once source and destination pixels differ.
      const float geometryTerm =
          max(0.0, dot(payload.ptRcSearch.prevGeometricNormal, previousDir)) / previousDistSq;

      payload.ptRcSearch.jacobianTerms = payload.ptRcSearch.prevSamplePdf * geometryTerm * vertexSamplePdf;
    }
  }

  // Tracked for the next vertex, which needs this vertex's criteria input and the
  // density/lobe of the event leaving it. The shared payload already carries the
  // predecessor position, but none of the rest.
  payload.ptRcSearch.prevRoughness       = surface.roughness;
  payload.ptRcSearch.prevLobeKind        = sampledLobeKind;
  payload.ptRcSearch.prevGeometricNormal = surface.geometricNormal;
  {
    float leavingPdf = 0.0;
    EvaluateSurfaceBsdfGroup(surface, viewDir, bounceDir, sampledLobeKind, leavingPdf);
    payload.ptRcSearch.prevSamplePdf = leavingPdf;
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
