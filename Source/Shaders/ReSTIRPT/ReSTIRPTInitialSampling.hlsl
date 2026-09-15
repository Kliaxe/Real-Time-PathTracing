// Initial sampling
// ReSTIR PT Enhanced. Generates one path tree per pixel and resamples it down to a single representative path (paper Section 2.3, with direct light unified into the same tree per Section 6.1).
// The path tracing itself is the shared code used by the reference path tracer, reused verbatim through PATH_TRACING_CUSTOM_GLOBALS. The only behavioral difference is the contribution hook: where the reference tracer sums contributions into radiance, this pass additionally streams each one through reservoir sampling.
// Because the hook only observes, payload.radiance still holds the exact reference answer for this sample, which the debug flag can output directly for an in-pass comparison against the resampled estimate.

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

// Path tracing hooks
// These macros customize the shared path tracing headers included after them.

// The reservoir travels with the path so every contribution site can reach it.
// ptResamplingSeed is deliberately separate from payload.seed: consuming the path tracer's RNG stream for reservoir selection would shift the sampling sequence and break bit-exact parity with the reference tracer for the same pixel/frame.
#define PATH_TRACING_PAYLOAD_EXTRA_FIELDS \
  ReSTIRPTReservoir          ptReservoir; \
  ReSTIRPTReconnectionSearch ptRcSearch;  \
  uint              ptResamplingSeed;     \
  float3            ptPrimaryGeometricNormal; \
  float3            ptPrimaryAlbedo;      \
  float             ptPrimaryLinearDepth;

// Every radiance contribution the tree produces becomes one resampling candidate with a unit candidate weight.
#define PATH_TRACING_ON_CONTRIBUTION(payload, contribution)                                              \
  PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(payload, contribution, 1.0)

// A contribution that arrives with its own unbiased contribution weight; RIS-based NEE is the only such site today. The weight belongs in the resampling weight, not in the stored integrand, which is why it is threaded through rather than folded in at the call site.
// NextRandom is drawn here rather than inside the reservoir so the resampling stream stays visibly separate from the path tracer's own.
// The reconnection vertex is attached at the moment of selection, not at the end: the search state describes the prefix of the path as it stands right now, and by the time the tree finishes it describes a different path.
#define PATH_TRACING_ON_CONTRIBUTION_WEIGHTED(payload, contribution, candidateWeight)                    \
  if(StreamPTCandidate(payload.ptReservoir, contribution, candidateWeight,                              \
                       NextRandom(payload.ptResamplingSeed)))                                            \
  {                                                                                                      \
    ApplyPTReconnectionToReservoir(payload.ptReservoir, payload.ptRcSearch, payload.depth);              \
  }

// Records the emissive NEE sample so the contribution hook, firing immediately afterwards, can adopt the light vertex as this path's reconnection anchor.
#define PATH_TRACING_ON_NEE_EMISSIVE(payload, lightIndex, barycentrics, radiance, lightPdf, distance)    \
  payload.ptRcSearch.neeLightIndex   = lightIndex;                                                       \
  payload.ptRcSearch.neeBarycentrics = float2(barycentrics.y, barycentrics.z);                           \
  payload.ptRcSearch.neeRadiance     = radiance;                                                         \
  payload.ptRcSearch.neePdf          = lightPdf;                                                         \
  payload.ptRcSearch.neeShadingDepth = payload.depth;                                                    \
  payload.ptRcSearch.neeValid        = 1u;

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

  return surface;
}

// The reduced primary-surface record later passes use for similarity tests and surface recovery. Invalid when the camera ray missed.
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
  const uint2         launchID   = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo  = pushConst.sceneInfoAddress.Get();
  const uint          width      = uint(sceneInfo.viewportSize.x);
  const uint          pixelIndex = launchID.y * width + launchID.x;

  RayDesc ray;

  ray.Origin    = sceneInfo.cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel((float2)launchID + 0.5, sceneInfo);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  // Seeding matches the reference tracer exactly so the two produce the same path for the same pixel and frame.
  const uint initialSeed = XxHash32(uint3(launchID, pushConst.rngFrameNumber));

  // Payload
  // The shared fields start exactly as the reference tracer's do; the ReSTIR fields start with an empty reservoir that remembers the sampling origin replay will regenerate the path from.

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

  payload.ptReservoir                = EmptyPTReservoir();
  payload.ptReservoir.initRandomSeed = initialSeed;
  payload.ptReservoir.samplePixel    = PackPathSamplePixel(launchID);
  payload.ptReservoir.sampleFrame    = pushConst.rngFrameNumber;
  payload.ptRcSearch                 = EmptyPTReconnectionSearch();

  // Offsetting the frame number decorrelates reservoir selection from path sampling; they must not share a stream.
  payload.ptResamplingSeed         = XxHash32(uint3(launchID, pushConst.rngFrameNumber + 0x9E3779B9u));
  payload.ptPrimaryGeometricNormal = (float3)0.0;
  payload.ptPrimaryAlbedo          = (float3)0.0;
  payload.ptPrimaryLinearDepth     = 0.0;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, payload);

  // One path tree is one sample of path space regardless of how many candidates it contained, so the confidence weight is 1 rather than the candidate count.
  FinalizePTReservoir(payload.ptReservoir, 1.0);

  // Correctness gate. Replacing the resampled sample with the reference sum, with ucw set to 1, sends the plain path-traced answer down the exact same storage, resolve, and accumulation path as the resampled estimate. Toggling the flag therefore isolates the resampling step and nothing else: with reuse disabled the two must converge to the same image.
  if((pushConst.flags & uint(ReSTIRPTFlags::eReSTIRPTFlagReferenceRadiance)) != 0u)
  {
    payload.ptReservoir.F         = max(payload.radiance, (float3)0.0);
    payload.ptReservoir.ucw       = 1.0;
    payload.ptReservoir.targetPdf = ReSTIRLuminance(payload.ptReservoir.F);
    payload.ptReservoir.M         = 1.0;
  }

  // Store
  // Section 6.3. With no reuse yet there is one candidate, so the vector weight is just its contribution; writing it here keeps final shading uniform across resampling modes instead of special-casing "no reuse ran".

  currentSurfaceBuffer[pixelIndex]  = BuildPTSurfaceFromPayload(payload);
  ptShadingWeightBuffer[pixelIndex] = payload.ptReservoir.F * payload.ptReservoir.ucw;

  StorePTReservoir(payload.ptReservoir, ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(launchID), ptParams.bufferIndices.initialSamplingOutputBufferIndex);

  // Denoiser guides
  // Hands final shading what only this pass can know about the path it traced.
  // A first bounce that was sampled but escaped the scene still reports a distance: the transport is real and merely far away. A lobe that was never sampled reports zero, which is what NRD defines as "no hit distance for this signal" and repairs from neighbouring pixels (see hitDistanceReconstructionMode in ReSTIRPTSettings).
  // The two cases must not be conflated: pretending an unsampled lobe was a distant hit would feed the denoiser a fabricated blur radius.

  if(payload.awaitingFirstBounceHitDistance != 0u)
  {
    StoreFirstBounceHitDistance(payload, kDenoiserFarHitDistance);
  }

  // Specular energy share
  // How much of this path's energy arrived through the specular lobe. An energy ratio rather than the lobe flag: the flag is one Bernoulli draw, whereas every contribution the tree collected (each NEE event, each bounce) lands in one of these two sums, so the ratio moves continuously with what the path actually carried.
  // Negative marks a path that collected nothing, where the ratio has no meaning and final shading falls back to the surface's own material response.

  const float diffuseLuminance  = ReSTIRLuminance(max(payload.diffuseDenoiserRadiance, (float3)0.0));
  const float specularLuminance = ReSTIRLuminance(max(payload.specularDenoiserRadiance, (float3)0.0));
  const float totalLuminance    = diffuseLuminance + specularLuminance;

  ptDenoiserGuideBuffer[pixelIndex] = float3(payload.hasDiffuseDenoiserHitDistance != 0u ? payload.diffuseDenoiserHitDistance : 0.0, payload.hasSpecularDenoiserHitDistance != 0u ? payload.specularDenoiserHitDistance : 0.0, totalLuminance > 0.0 ? (specularLuminance / totalLuminance) : -1.0);
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

  // Tagged before publishing, so a selected escape is recorded as an environment-miss endpoint that replay knows to let miss.
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
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();
  const SurfaceData   surface   = LoadSurfaceData(attr);
  const float3        viewDir   = -WorldRayDirection();

  // Captured before the bounce increments it, so it names this vertex.
  const uint vertexDepth = payload.depth;

  // Capture the extra primary-surface terms the shared helper does not carry. Later passes need geometry and albedo for surface similarity and shift tests.
  if(payload.hasPrimarySurface == 0u)
  {
    payload.ptPrimaryGeometricNormal = surface.geometricNormal;
    payload.ptPrimaryAlbedo          = surface.albedo;
    payload.ptPrimaryLinearDepth     = length(surface.worldPosition - sceneInfo.cameraPosition);
  }

  StorePrimarySurfaceForDenoiser(payload, surface);
  StoreFirstBounceHitDistance(payload, RayTCurrent());

  ApplyCurrentMediumAttenuation(payload, RayTCurrent());

  // Emission
  // Emission found by a BSDF-sampled ray is MIS-weighted against light sampling of the same triangle.

  float emissiveMisWeight = 1.0;

  // Only when the previous vertex took an emissive light sample; otherwise nothing makes up the weight taken away here. PTShift's replay applies the same test.
  if(payload.hasLastBsdfSample != 0 && payload.lastEmissiveNeeActive != 0 && SafeMax3(surface.emission) > 0.0)
  {
    const float lightPdf = EvaluateCurrentEmissiveHitPdf(sceneInfo, InstanceIndex(), PrimitiveIndex(), payload.lastSurfacePosition, surface.worldPosition);

    if(lightPdf > 0.0 && payload.lastDirectLightBsdfPdf > 0.0)
    {
      emissiveMisWeight = MisMixWeight(payload.lastDirectLightBsdfPdf, lightPdf);
    }
  }

  // Tag the site before it publishes: the selection hook fires inside these calls and has no other way to know which estimator produced the candidate.
  payload.ptRcSearch.pendingEndpointKind = RESTIR_PT_ENDPOINT_KIND_BSDF_EMISSIVE;

  AccumulatePathContribution(payload, payload.throughput * surface.emission * emissiveMisWeight);

  // Next event estimation
  // At the primary hit this is the direct lighting that a separate ReSTIR DI pass would otherwise own; here it simply joins the same path tree as a length-2 candidate (Section 6.1).
  // NEE draws from this vertex's own stream so its consumption cannot shift the BSDF sample below, which replay must reproduce exactly.
  // Section 6.1. The RIS variant consumes a different number of random values than the single-sample one, which is safe only because the BSDF sample below is re-seeded from its own per-vertex stream rather than continuing this one. Replay never re-executes NEE at an emissive endpoint (that endpoint carries a forced anchor and is shifted geometrically), so the two variants stay interchangeable from the shift's point of view.

  payload.seed = MakePathSampleStream(payload.ptReservoir.samplePixel, payload.ptReservoir.sampleFrame, vertexDepth, kPTStreamNee);

  payload.ptRcSearch.pendingEndpointKind = RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT;

  AccumulateEnvironmentDirectLight(payload, surface, sceneInfo, viewDir);

  payload.ptRcSearch.pendingEndpointKind = RESTIR_PT_ENDPOINT_KIND_EMISSIVE_NEE;

  payload.seed = MakePathSampleStream(payload.ptReservoir.samplePixel, payload.ptReservoir.sampleFrame, vertexDepth, kPTStreamEmissive);

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

  // BSDF sample
  // Replay regenerates these exact lobe and direction dimensions. Roulette has its own stream because replay accounts for its density without redrawing survival.

  payload.seed = MakePathSampleStream(payload.ptReservoir.samplePixel, payload.ptReservoir.sampleFrame, vertexDepth, kPTStreamBsdf);

  float3 bounceDir;
  float3 bsdfOverPdf         = (float3)0.0;
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

  // Russian roulette (Section 6.2.4). Applied only here, during initial sampling. It must never run during random replay, because killing a path the base path survived makes the shift fail for reasons unrelated to the surfaces involved.
  if(ptParams.initialSampling.enableRussianRoulette != 0u && payload.depth >= ptParams.initialSampling.russianRouletteStartBounce)
  {
    const float continueProbability = clamp(SafeMax3(nextThroughput), 0.05, 0.95);

    payload.seed = MakePathSampleStream(payload.ptReservoir.samplePixel, payload.ptReservoir.sampleFrame, vertexDepth, kPTStreamRoulette);

    if(NextRandom(payload.seed) > continueProbability)
    {
      return;
    }

    nextThroughput /= continueProbability;
  }

  // Leaving density
  // The density of the group the sampler chose, recovered by re-evaluating it for the sampled direction. The reconnection criteria and the Jacobian's base denominator below need it as this vertex's outgoing density, and the next vertex needs it as its predecessor's, so it is evaluated once for both.

  float vertexSamplePdf = 0.0;

  EvaluateSurfaceBsdfGroup(surface, viewDir, bounceDir, sampledLobeKind, vertexSamplePdf);

  // Reconnection vertex search
  // Paper Section 2.3. The first vertex whose pair with its predecessor passes the criteria wins and is never revisited: fixing the reconnection point early is what bounds the replay length and keeps the shift's cost predictable.
  // Requires vertexDepth >= 1 because the criteria compare two surfaces, and the predecessor of the primary hit is the camera, which has no roughness.

  if(payload.ptRcSearch.found == 0 && vertexDepth >= 1)
  {
    const float connectionDistance = length(surface.worldPosition - payload.lastSurfacePosition);

    bool qualifies;

    if(ptParams.shift.reconnectionCriteria == RESTIR_PT_RECONNECTION_CRITERIA_FOOTPRINT)
    {
      // Section 4.2's single-vertex roughness threshold is retained alongside the footprint test, guarding cases the footprint bound cannot cover: parallax, curvature, and reconnection to lights at infinity.
      const bool passesRoughnessGuard = payload.ptRcSearch.prevRoughness >= ptParams.shift.minRoughness;

      // Footnote 6: a broad-group (diffuse-like) or emissive reconnection vertex does not change its outgoing density under reconnection, so the inverse half is skipped.
      const bool skipInverseFootprint = (sampledLobeKind == 0u) || SafeMax3(surface.emission) > 0.0;

      qualifies = passesRoughnessGuard && PassesFootprintReconnectionCriteria(payload.ptRcSearch.prevSamplePdf, vertexSamplePdf, payload.lastSurfacePosition, payload.ptRcSearch.prevGeometricNormal, surface.worldPosition, surface.geometricNormal, payload.primaryWorldPosition, payload.ptPrimaryGeometricNormal, sceneInfo.cameraPosition, ptParams.shift.footprintThreshold, skipInverseFootprint);
    }
    else
    {
      qualifies = PassesLegacyReconnectionCriteria(payload.ptRcSearch.prevRoughness, surface.roughness, connectionDistance, ptParams.shift.minRoughness, ptParams.shift.legacyMinDistance);
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

      // Frozen here, not read at contribution time: every later vertex overwrites the running prev* fields.
      payload.ptRcSearch.rcPrevLobeKind = payload.ptRcSearch.prevLobeKind;

      // Post-roulette throughput, so dividing a later integrand by it recovers exactly the suffix radiance the shift will reuse.
      payload.ptRcSearch.suffixThroughput = nextThroughput;

      // Base Jacobian denominator
      // Equation 2's D_x is assembled while both vertices are still in hand. The density at this vertex is the vertexSamplePdf recovered above by re-evaluating the group the sampler chose, which reproduces the very pdf it divided by.
      // The geometry term is single-sided, measured at the source vertex, matching the paper's definition. For a self-shift the convention cancels; it only starts to matter once source and destination pixels differ.

      const float3 fromPrevious   = surface.worldPosition - payload.lastSurfacePosition;
      const float  previousDistSq = max(dot(fromPrevious, fromPrevious), 1.0e-8);
      const float3 previousDir    = fromPrevious * rsqrt(previousDistSq);
      const float  geometryTerm   = max(0.0, dot(payload.ptRcSearch.prevGeometricNormal, previousDir)) / previousDistSq;

      payload.ptRcSearch.jacobianTerms = payload.ptRcSearch.prevSamplePdf * geometryTerm * vertexSamplePdf;
    }
  }

  // Predecessor state
  // Tracked for the next vertex, which needs this vertex's criteria input and the density/lobe of the event leaving it. The shared payload already carries the predecessor position, but none of the rest.

  payload.ptRcSearch.prevRoughness       = surface.roughness;
  payload.ptRcSearch.prevLobeKind        = sampledLobeKind;
  payload.ptRcSearch.prevGeometricNormal = surface.geometricNormal;
  payload.ptRcSearch.prevSamplePdf       = vertexSamplePdf;

  // Continue the path
  // Identical to the reference tracer from here: update the medium, record what the next vertex's MIS needs, and trace the bounce.

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
