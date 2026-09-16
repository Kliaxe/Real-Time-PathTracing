#ifndef RESTIR_PT_SHIFT_H
#define RESTIR_PT_SHIFT_H

// Hybrid shift mapping
// Maps a path sampled in one pixel's domain into another's (paper Section 2.3): replay the prefix from the base path's random numbers, then reconnect geometrically at the stored reconnection vertex and reuse the suffix radiance recorded there.
// Equation 2's Jacobian is the ratio of the reconnection densities in the two domains.
// Included by every pass that reuses paths. The including shader must already have pulled in PTGlobals, PTReservoir, and the shared path tracing headers, and must include PathRayEntryPoints.hlsli: TracePTVertex traces the same path rays the estimator does, through the same hit and miss shaders.

// Independent integer streams for reservoir selection and neighbor selection.
uint PTVertexSeed(uint initialSeed, uint vertexDepth, uint purpose)
{
  return XxHash32(uint3(initialSeed, vertexDepth, purpose));
}

// PTVertexQuery
// One traced ray that returns the surface it hit. Used for both the primary ray and every replayed bounce.

struct PTVertexQuery
{
  // Full material record at the hit; undefined when hit is false.
  SurfaceData surface;
  // Whether the ray found geometry.
  bool hit;
  // InstanceIndex() of the hit, which reconnection compares against the stored vertex identity.
  uint instanceId;
  // PrimitiveIndex() of the hit, compared the same way.
  uint primitiveIndex;
};

// Traces one ray and resolves the surface it found.
// The ray carries the same 24-byte PathHitRecord the estimator's rays do, and the surface is built here in ray generation rather than in closest hit: a payload carrying a whole SurfaceData was 184 bytes copied into and out of every replayed bounce.
PTVertexQuery TracePTVertex(float3 origin, float3 direction)
{
  RayDesc ray;

  ray.Origin    = origin;
  ray.Direction = direction;
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  PathHitRecord hit;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, hit);

  PTVertexQuery query;

  query.hit            = hit.hasHit != 0u;
  query.instanceId     = ReSTIRPTInvalidInstanceId;
  query.primitiveIndex = 0u;

  // A missed ray leaves the surface undefined, exactly as the payload did: every caller tests hit before reading it.
  if(query.hit)
  {
    query.surface        = LoadSurfaceDataFromHit(hit, direction);
    query.instanceId     = hit.instanceIndex;
    query.primitiveIndex = hit.primitiveIndex;
  }

  return query;
}

// Rebuilds the full material record at a primary hit some pass already stored, as the camera that saw it would have resolved it.
// The stored record names the triangle and the point on it, so the surface comes straight from the scene buffers. Every reuse pass used to trace a ray back at the stored point for this instead: one per destination pixel, per history sample, and per neighbour.
// The direction is the one that found the hit, because the loader face-forwards the frame against it, and the view direction the shift evaluates BSDFs with is its opposite.
// No "did the ray still reach the stored point" guard is needed, unlike the trace this replaces: the loaded hit IS the stored one, so nothing nearer can stand in for it. That holds because geometry is static for the lifetime of a surface buffer, and a scene rebuild invalidates the buffers along with the history they describe.
bool LoadPTSurfaceAtStoredHit(ReSTIRPTSurface stored, float3 cameraPosition, out SurfaceData surface, out float3 viewDir)
{
  surface = (SurfaceData)0;
  viewDir = float3(0.0, 1.0, 0.0);

  // A record whose primary ray escaped names no triangle.
  if(stored.valid == 0)
  {
    return false;
  }

  PathHitRecord hit;

  hit.instanceIndex  = stored.instanceIndex;
  hit.primitiveIndex = stored.primitiveIndex;
  hit.barycentrics   = stored.barycentrics;
  hit.hitDistance    = stored.linearDepth;
  hit.hasHit         = 1u;

  const float3 direction = normalize(stored.worldPosition - cameraPosition);

  surface = LoadSurfaceDataFromHit(hit, direction);
  viewDir = -direction;

  return true;
}

// PTShiftResult
// Everything resampling needs from one shift: the integrand in the destination domain, the Jacobian that weights it, and why the shift ended the way it did.

struct PTShiftResult
{
  // Shifted integrand F in the destination domain, RGB.
  float3 integrand;
  // Equation 2's Jacobian D_y / D_x; 1 for shifts that are an identity in primary sample space.
  float jacobian;
  // Equation 2's denominator evaluated in the DESTINATION domain (D_y). Exposed separately from the ratio because a path that survives resampling is stored in the destination domain, and the next frame's shift must divide by D_y rather than by the source denominator the path was originally built against. Without rebasing this, a chain of reuses computes every Jacobian against a stale base.
  float destinationDenominator;
  // Equation 2's denominator in the SOURCE domain (D_x), i.e. the stored base value the ratio divides by. Exposed only so a diagnostic can decompose an extreme Jacobian into which side is degenerate.
  float sourceDenominator;
  // Whether the shift produced a usable candidate. A failed shift is a null candidate, not an absent technique.
  bool valid;
  // ReSTIRPTShiftOutcome. A bare success/fail flag cannot separate an occluded reconnection from an unsupported lobe, and those want opposite fixes.
  uint outcome;
};

PTShiftResult FailedPTShift(uint outcome)
{
  PTShiftResult result;

  result.integrand              = (float3)0.0;
  result.jacobian               = 0.0;
  result.destinationDenominator = 0.0;
  result.sourceDenominator      = 0.0;
  result.valid                  = false;
  result.outcome                = outcome;

  return result;
}

// Russian roulette during replay: survive DETERMINISTICALLY, but still apply its density.
// The supplemental removes the roulette dimensions from the shift's parameter space and deterministically selects survival, which is why no random number is drawn.
// The division must stay, though: the base path scaled its throughput by 1/survival at every vertex past the start bounce, so skipping it makes the replayed path darker than the one it is meant to reproduce.
// resultingDepth is the path depth AFTER the bounce being accounted for, matching how initial sampling increments depth before testing the start bounce.
void ApplyReplayRouletteDensity(inout float3 throughput, uint resultingDepth)
{
  if(ptParams.initialSampling.enableRussianRoulette == 0u || resultingDepth < ptParams.initialSampling.russianRouletteStartBounce)
  {
    return;
  }

  const float continueProbability = clamp(SafeMax3(throughput), 0.05, 0.95);

  throughput /= continueProbability;
}

// Whether the offset path would itself have chosen this pair as its reconnection vertex, evaluated with the same criteria initial sampling used on the base path.
// ReSTIR PT Section 7.4 requires this: the base path selects the FIRST vertex that qualifies, so a shift is only invertible if the offset path agrees on which vertex that is. "When building y, if we find it disagrees on the earliest possible reconnection vertex, the shift must return undefined as it would not be invertible."
// The criteria are evaluated entirely in the destination domain - its primary hit sets the footprint scale - because that is the path whose reconnection choice is in question.
bool OffsetPathQualifiesForReconnection(SurfaceData previousSurface, uint previousLobeKind, float previousSamplePdf, SurfaceData vertexSurface, uint vertexLobeKind, float vertexSamplePdf, SurfaceData destinationSurface)
{
  const GltfSceneInfo sceneInfo          = pushConst.sceneInfoAddress.Get();
  const float         previousRoughness  = SurfaceBsdfGroupRoughness(previousSurface, previousLobeKind);

  if(ptParams.shift.reconnectionCriteria != RESTIR_PT_RECONNECTION_CRITERIA_FOOTPRINT)
  {
    const float connectionDistance = length(vertexSurface.worldPosition - previousSurface.worldPosition);

    return PassesLegacyReconnectionCriteria(previousRoughness, SurfaceBsdfGroupRoughness(vertexSurface, vertexLobeKind), connectionDistance, ptParams.shift.minRoughness, ptParams.shift.legacyMinDistance);
  }

  const bool skipInverseFootprint = (vertexLobeKind == 0u) || SafeMax3(vertexSurface.emission) > 0.0;

  return previousRoughness >= ptParams.shift.minRoughness && PassesFootprintReconnectionCriteria(previousSamplePdf, vertexSamplePdf, previousSurface.worldPosition, previousSurface.geometricNormal, vertexSurface.worldPosition, vertexSurface.geometricNormal, destinationSurface.worldPosition, destinationSurface.geometricNormal, sceneInfo.cameraPosition, ptParams.shift.footprintThreshold, skipInverseFootprint);
}

// Shifts a path that carries no reconnection anchor, by replaying it to its end.
// A path only reaches here when no vertex qualified for reconnection: in practice the short ones, where the contribution happens at or just past the primary hit and there is no interior vertex to anchor on. Without this they cannot be reused at all, which is why they dominate the remaining shift failures.
// The mapping is the identity in primary sample space: every vertex is regenerated from the same per-vertex random stream the base path drew from, so no coordinate is remapped and the Jacobian is exactly 1. What differs is the surfaces those coordinates land on, which is precisely the reuse being performed.
// The endpoint estimator must be reproduced, not approximated: emission found by a BSDF ray carries a MIS weight against light sampling, and an explicitly sampled environment direction carries the reciprocal weight. Evaluating the wrong one double counts or drops energy rather than merely adding noise.
PTShiftResult FullReplayPathToSurface(ReSTIRPTReservoir reservoir, SurfaceData destinationSurface, float3 destinationViewDir)
{
  const uint endpointDepth = (reservoir.pathFlags & RESTIR_PT_PATH_FLAGS_ENDPOINT_DEPTH_MASK) >> RESTIR_PT_PATH_FLAGS_ENDPOINT_DEPTH_SHIFT;
  const uint endpointKind  = (reservoir.pathFlags & RESTIR_PT_PATH_FLAGS_ENDPOINT_KIND_MASK) >> RESTIR_PT_PATH_FLAGS_ENDPOINT_KIND_SHIFT;

  // Refused before any tracing: a disabled kind must cost nothing, so the mask reads the same as the unimplemented case did.
  if((ptParams.shift.replayEndpointMask & (1u << endpointKind)) == 0u)
  {
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoReconnection));
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();

  // Walk state
  // lastBsdfPdf, lastVertexOrigin, hasLastBsdfSample and lastEmissiveNeeActive describe the BSDF sample that produced the current vertex. The emissive endpoint's MIS weight is built from them, and they are only knowable while walking.
  // endpointInstanceId and endpointPrimitiveIndex identify the vertex the replay ended on, needed to evaluate the light sampling density of the emissive triangle actually hit.

  SurfaceData currentSurface = destinationSurface;
  float3      currentViewDir = destinationViewDir;
  float3      throughput     = (float3)1.0;

  float  lastBsdfPdf           = 0.0;
  float3 lastVertexOrigin      = destinationSurface.worldPosition;
  bool   hasLastBsdfSample     = false;
  bool   lastEmissiveNeeActive = false;

  uint endpointInstanceId     = ReSTIRPTInvalidInstanceId;
  uint endpointPrimitiveIndex = 0u;

  // An escaping endpoint has no surface at endpointDepth: the depth counts the bounce that left the scene, so the walk stops at the vertex it left from.
  const bool endpointEscapes = endpointKind == RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT_MISS;
  const uint walkDepth       = endpointEscapes ? (endpointDepth > 0u ? endpointDepth - 1u : 0u) : endpointDepth;

  // Depth 0 means the camera ray itself missed, which is background rather than a reusable path.
  if(endpointEscapes && endpointDepth == 0u)
  {
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoReconnection));
  }

  // Prefix walk
  // Each iteration regenerates one vertex from its own BSDF stream and traces to the next surface.

  for(uint vertexDepth = 0; vertexDepth < walkDepth; ++vertexDepth)
  {
    PathSampleStream seed = MakePathSampleStream(reservoir.samplePixel, reservoir.sampleFrame, vertexDepth, kPTStreamBsdf);
    float3 bounceDir;
    float3 bsdfOverPdf         = (float3)0.0;
    float  directLightBsdfPdf  = 0.0;
    bool   isTransmissionEvent = false;
    uint   sampledLobeKind     = 0u;

    if(!SampleSurfaceBsdf(currentSurface, currentViewDir, seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplaySampleFail));
    }

    throughput *= max(bsdfOverPdf, (float3)0.0);

    if(SafeMax3(throughput) <= 0.0)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplaySampleFail));
    }

    ApplyReplayRouletteDensity(throughput, vertexDepth + 1u);

    const float3 originNormal = SelectOffsetNormal(currentSurface.geometricNormal, bounceDir);

    lastVertexOrigin      = currentSurface.worldPosition;
    lastEmissiveNeeActive = CanSampleEmissiveDirectLight(currentSurface, sceneInfo);

    const PTVertexQuery next = TracePTVertex(OffsetRay(currentSurface.worldPosition, originNormal), bounceDir);

    // The base path found geometry here. An escaped replay has no counterpart vertex, so there is nothing to evaluate the endpoint against.
    if(!next.hit)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplayEscaped));
    }

    currentSurface         = next.surface;
    currentViewDir         = -bounceDir;
    lastBsdfPdf            = directLightBsdfPdf;
    hasLastBsdfSample      = true;
    endpointInstanceId     = next.instanceId;
    endpointPrimitiveIndex = next.primitiveIndex;
  }

  // Endpoint evaluation
  // Identity in primary sample space, so every denominator is 1; see above.

  PTShiftResult result;

  result.jacobian               = 1.0;
  result.destinationDenominator = 1.0;
  result.sourceDenominator      = 1.0;
  result.integrand              = (float3)0.0;

  if(endpointKind == RESTIR_PT_ENDPOINT_KIND_BSDF_EMISSIVE)
  {
    // Mirrors the accumulation site in initial sampling, including the MIS weight that balances this against the light-sampling estimator for the same vertex.
    float emissiveMisWeight = 1.0;

    if(hasLastBsdfSample && lastEmissiveNeeActive && SafeMax3(currentSurface.emission) > 0.0 && lastBsdfPdf > 0.0)
    {
      const float lightPdf = EvaluateCurrentEmissiveHitPdf(sceneInfo, endpointInstanceId, endpointPrimitiveIndex, lastVertexOrigin, currentSurface.worldPosition);

      if(lightPdf > 0.0)
      {
        emissiveMisWeight = MisMixWeight(lastBsdfPdf, lightPdf);
      }
    }

    result.integrand = throughput * currentSurface.emission * emissiveMisWeight;
  }
  else if(endpointKind == RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT)
  {
    // Redraw the environment direction from the endpoint vertex's own NEE stream, which is the same coordinate the base path used.
    PathSampleStream seed     = MakePathSampleStream(reservoir.samplePixel, reservoir.sampleFrame, endpointDepth, kPTStreamNee);
    float3 lightDir = (float3)0.0;
    float3 radiance = (float3)0.0;

    float3 receiverNormal = currentSurface.shadingNormal;

    if(CanSampleEnvironmentTransmissionExitLight(currentSurface))
    {
      receiverNormal = -currentSurface.shadingNormal;
    }
    else if(!CanSampleEnvironmentReflectionDirectLight(currentSurface))
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomePrevLobeUnsupported));
    }

    if(!SampleEnvironmentLightDirection(sceneInfo, receiverNormal, seed, lightDir, radiance))
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplaySampleFail));
    }

    if(!TraceVisibility(currentSurface.worldPosition, currentSurface.geometricNormal, lightDir))
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeOccluded));
    }

    float        bsdfPdf = 0.0;
    const float3 bsdf    = EvaluateDirectLightBsdf(currentSurface, currentViewDir, lightDir, bsdfPdf);

    if(bsdfPdf <= 0.0 || SafeMax3(bsdf) <= 0.0)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomePrevLobeUnsupported));
    }

    const float lightPdf = EvaluateEnvironmentLightPdf(sceneInfo, receiverNormal, lightDir);

    if(lightPdf <= 0.0)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeBadDenominator));
    }

    const float misWeight = MisMixWeight(lightPdf, bsdfPdf);

    result.integrand = throughput * radiance * bsdf * (misWeight / lightPdf);
  }
  else if(endpointKind == RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT_MISS)
  {
    // One more bounce from the vertex the base path escaped from, drawn from that vertex's own stream. Here a miss is the expected outcome and a hit is the failure: the two domains disagree about whether the path leaves the scene.
    PathSampleStream seed = MakePathSampleStream(reservoir.samplePixel, reservoir.sampleFrame, walkDepth, kPTStreamBsdf);
    float3 bounceDir;
    float3 bsdfOverPdf         = (float3)0.0;
    float  directLightBsdfPdf  = 0.0;
    bool   isTransmissionEvent = false;
    uint   sampledLobeKind     = 0u;

    if(!SampleSurfaceBsdf(currentSurface, currentViewDir, seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplaySampleFail));
    }

    throughput *= max(bsdfOverPdf, (float3)0.0);

    if(SafeMax3(throughput) <= 0.0)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplaySampleFail));
    }

    ApplyReplayRouletteDensity(throughput, walkDepth + 1u);

    const float3        originNormal = SelectOffsetNormal(currentSurface.geometricNormal, bounceDir);
    const PTVertexQuery escape       = TracePTVertex(OffsetRay(currentSurface.worldPosition, originNormal), bounceDir);

    if(escape.hit)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplayEscaped));
    }

    // MIS against the explicit environment sample that could have found the same direction, matching the miss shader. The pairing only applies where that explicit sample was actually taken.
    float misWeight = 1.0;

    if(CanSampleEnvironmentReflectionDirectLight(currentSurface) || CanSampleEnvironmentTransmissionExitLight(currentSurface))
    {
      const float lightPdf = EvaluateEnvironmentLightPdf(sceneInfo, currentSurface.shadingNormal, bounceDir);

      if(lightPdf > 0.0 && directLightBsdfPdf > 0.0)
      {
        misWeight = MisMixWeight(directLightBsdfPdf, lightPdf);
      }
    }

    result.integrand = throughput * SampleEnvironment(sceneInfo, bounceDir) * misWeight;
  }
  else
  {
    // An emissive-NEE endpoint always carries a forced anchor, and an interior contribution is handled by the hybrid shift, so neither should arrive here.
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoReconnection));
  }

  result.valid   = SafeMax3(result.integrand) > 0.0;
  result.outcome = result.valid ? uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeSuccess) : uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeBadDenominator);

  return result;
}

// Shifts one stored path into the domain of a destination primary surface.
// Structure follows the hybrid shift (paper Section 2.3): replay the prefix using the base path's random numbers, then reconnect geometrically at the stored reconnection vertex and reuse the suffix radiance recorded there.
PTShiftResult ShiftPathToSurface(ReSTIRPTReservoir reservoir, SurfaceData destinationSurface, float3 destinationViewDir)
{
  const bool hasReconnection = (reservoir.pathFlags & RESTIR_PT_PATH_FLAGS_HAS_RC_VERTEX) != 0u;

  // No anchor to reconnect at, so the path is replayed to its end instead. Gated so the hybrid-only behaviour stays reproducible for comparison.
  if(!hasReconnection)
  {
    if(ptParams.shift.replayEndpointMask == 0u)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoReconnection));
    }

    return FullReplayPathToSurface(reservoir, destinationSurface, destinationViewDir);
  }

  const uint reconnectionLength = (reservoir.pathFlags & RESTIR_PT_PATH_FLAGS_RC_LENGTH_MASK) >> RESTIR_PT_PATH_FLAGS_RC_LENGTH_SHIFT;

  // An NEE endpoint's anchor is forced to the light vertex (Section 6.2.3) in preference to any interior vertex that qualified, so the criteria never chose it and both domains reach the same anchor by the same rule.
  // The agreement tests below therefore do not apply to these paths: refusing one because the replayed prefix happens to qualify somewhere would reject a shift that is already a bijection.
  const bool isNeeEndpoint = (reservoir.pathFlags & RESTIR_PT_PATH_FLAGS_NEE_ENDPOINT) != 0u;

  // Prefix walk
  // Vertex 0 is the destination primary hit, which the caller already has; each iteration regenerates one more vertex toward the reconnection vertex's predecessor.
  // Russian roulette is never sampled here (Section 6.2.4): killing a path the base path survived would fail the shift for reasons unrelated to the surfaces involved. Only its density is applied, by ApplyReplayRouletteDensity.

  SurfaceData currentSurface = destinationSurface;
  float3      currentViewDir = destinationViewDir;
  float3      throughput     = (float3)1.0;

  // Predecessor state
  // Carried for the same reason initial sampling carries it: the reconnection criteria compare a pair of vertices, and the pair is only complete once the second one has been sampled.

  SurfaceData previousSurface   = destinationSurface;
  uint        previousLobeKind  = 0u;
  float       previousSamplePdf = 0.0;
  bool        hasPredecessor    = false;

  for(uint vertexDepth = 0; vertexDepth + 1 < reconnectionLength; ++vertexDepth)
  {
    PathSampleStream seed = MakePathSampleStream(reservoir.samplePixel, reservoir.sampleFrame, vertexDepth, kPTStreamBsdf);
    float3 bounceDir;
    float3 bsdfOverPdf         = (float3)0.0;
    float  directLightBsdfPdf  = 0.0;
    bool   isTransmissionEvent = false;
    uint   sampledLobeKind     = 0u;

    if(!SampleSurfaceBsdf(currentSurface, currentViewDir, seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplaySampleFail));
    }

    throughput *= max(bsdfOverPdf, (float3)0.0);

    if(SafeMax3(throughput) <= 0.0)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplaySampleFail));
    }

    ApplyReplayRouletteDensity(throughput, vertexDepth + 1u);

    // Reconnection agreement
    // The base path anchored on the first vertex that satisfied the criteria. If the replayed path satisfies them earlier, its own anchor would be a different vertex, so this shift is not the inverse of anything and Section 7.4 requires refusing it rather than evaluating it.

    float vertexSamplePdf = 0.0;

    EvaluateSurfaceBsdfGroup(currentSurface, currentViewDir, bounceDir, sampledLobeKind, vertexSamplePdf);

    if(!isNeeEndpoint && hasPredecessor && OffsetPathQualifiesForReconnection(previousSurface, previousLobeKind, previousSamplePdf, currentSurface, sampledLobeKind, vertexSamplePdf, destinationSurface))
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReconnectionDisagreement));
    }

    previousSurface   = currentSurface;
    previousLobeKind  = sampledLobeKind;
    previousSamplePdf = vertexSamplePdf;
    hasPredecessor    = true;

    const float3        originNormal = SelectOffsetNormal(currentSurface.geometricNormal, bounceDir);
    const PTVertexQuery next         = TracePTVertex(OffsetRay(currentSurface.worldPosition, originNormal), bounceDir);

    // The replayed path escaped where the base path found geometry, so the two domains disagree and the shift has no counterpart vertex.
    if(!next.hit)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReplayEscaped));
    }

    currentSurface = next.surface;
    currentViewDir = -bounceDir;
  }

  // NEE endpoint
  // An NEE endpoint anchors on the light vertex itself, which behaves differently from an interior reconnection: the suffix past it is empty, so there is no continuation direction and no BSDF to evaluate there.
  // Everything that depends on the vertex the shadow ray left from (the BSDF, the light's solid angle density, and the MIS weight against BSDF sampling) has to be rebuilt for the new origin, because only the emitted radiance is domain independent.

  if((reservoir.pathFlags & RESTIR_PT_PATH_FLAGS_NEE_ENDPOINT) != 0u)
  {
    const GltfSceneInfo neeSceneInfo = pushConst.sceneInfoAddress.Get();
    const uint          lightIndex   = reservoir.rcVertexInstanceId;

    if(lightIndex >= neeSceneInfo.emissiveTriangleCount)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoReconnection));
    }

    const EmissiveTriangleLight light         = LoadDeviceArrayElement<EmissiveTriangleLight>(neeSceneInfo.emissiveTriangles, lightIndex);
    const float2                uv            = reservoir.rcVertexBarycentrics;
    const float3                bary          = float3(1.0 - uv.x - uv.y, uv.x, uv.y);
    const float3                lightPosition = light.position0 * bary.x + light.position1 * bary.y + light.position2 * bary.z;

    // Same offset origin the light sampler used, so visibility, PDF and geometry all agree on where the ray starts.
    const float3 unoffsetToLight = lightPosition - currentSurface.worldPosition;

    if(dot(unoffsetToLight, unoffsetToLight) <= 1.0e-8)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeDegenerate));
    }

    const float3 shadowOrigin = OffsetRay(currentSurface.worldPosition, SelectOffsetNormal(currentSurface.geometricNormal, unoffsetToLight));
    const float3 toLight      = lightPosition - shadowOrigin;
    const float  distanceSq   = dot(toLight, toLight);

    if(distanceSq <= 1.0e-8)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeDegenerate));
    }

    const float  lightDistance = sqrt(distanceSq);
    const float3 lightDir      = toLight / lightDistance;

    const GltfMetallicRoughness lightMaterial = LoadDeviceArrayElement<GltfMetallicRoughness>(neeSceneInfo.materials, light.materialIndex);
    const float                 lightCos      = EvaluateEmissiveTriangleCosine(lightMaterial, light.geometricNormal, -lightDir);

    if(lightCos <= 0.0)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeDegenerate));
    }

    const float shiftedLightPdf = EvaluateEmissiveTriangleSolidAnglePdf(neeSceneInfo, lightIndex, light, shadowOrigin, lightPosition);

    if(shiftedLightPdf <= 0.0)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeBadDenominator));
    }

    if(!TraceVisibilityFromOrigin(shadowOrigin, lightDir, max(lightDistance - 0.001, 0.001)))
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeOccluded));
    }

    // The direct-light evaluator, not the full mixture: explicit light sampling only ever estimates the broad proposal group, so reproducing its estimator means evaluating exactly that group.
    float        shiftedBsdfPdf = 0.0;
    const float3 shiftedBsdf    = EvaluateDirectLightBsdf(currentSurface, currentViewDir, lightDir, shiftedBsdfPdf);

    if(shiftedBsdfPdf <= 0.0 || SafeMax3(shiftedBsdf) <= 0.0)
    {
      return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomePrevLobeUnsupported));
    }

    const float shiftedMisWeight = MisMixWeight(shiftedLightPdf, shiftedBsdfPdf);

    // NEE result
    // The light point is shared and its primary-sample coordinates are reused unchanged, so this mapping is an identity in primary sample space and contributes no Jacobian of its own (supplemental Section 1).

    PTShiftResult neeResult;

    neeResult.integrand              = throughput * reservoir.rcVertexRadiance * shiftedBsdf * (shiftedMisWeight / shiftedLightPdf);
    neeResult.jacobian               = 1.0;
    neeResult.destinationDenominator = 1.0;
    neeResult.sourceDenominator      = 1.0;
    neeResult.valid                  = SafeMax3(neeResult.integrand) > 0.0;
    neeResult.outcome                = neeResult.valid ? uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeSuccess) : uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeBadDenominator);

    return neeResult;
  }

  // Reconnection vertex position
  // Rebuilt from its stored identity rather than stored outright: GltfInstance carries a local-to-world transform, so instance + primitive + barycentrics locate it exactly for 16 bytes instead of 12 bytes of world position that would also lose precision at distance.

  const GltfSceneInfo sceneInfo  = pushConst.sceneInfoAddress.Get();
  const GltfInstance  rcInstance = LoadDeviceArrayElement<GltfInstance>(sceneInfo.instances, reservoir.rcVertexInstanceId);
  const GltfMesh      rcMesh     = LoadDeviceArrayElement<GltfMesh>(sceneInfo.meshes, rcInstance.meshIndex);
  const uint3         rcIndices  = getTriangleIndices(rcMesh.gltfBuffer, rcMesh.triMesh, reservoir.rcVertexPrimitiveIndex);
  const float2        rcUv       = reservoir.rcVertexBarycentrics;
  const float3        rcBary     = float3(1.0 - rcUv.x - rcUv.y, rcUv.x, rcUv.y);
  const float3        rcLocal    = getTriangleAttribute<float3>(rcMesh.gltfBuffer, rcMesh.triMesh.positions, rcIndices, rcBary);
  const float3        rcPosition = mul(float4(rcLocal, 1.0), rcInstance.transform).xyz;

  const float3 toReconnection  = rcPosition - currentSurface.worldPosition;
  const float  distanceSquared = dot(toReconnection, toReconnection);

  if(distanceSquared <= 1.0e-8)
  {
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeDegenerate));
  }

  const float  connectionDistance = sqrt(distanceSquared);
  const float3 connectionDir      = toReconnection / connectionDistance;

  // Connection ray
  // Tracing toward the vertex does double duty: it is the visibility test, and its hit supplies the full material data that the stored identity alone cannot reconstruct outside a hit shader.
  // Landing on different geometry means the reconnection is occluded from this domain and the shift has no counterpart.

  const float3        originNormal = SelectOffsetNormal(currentSurface.geometricNormal, connectionDir);
  const PTVertexQuery rcQuery      = TracePTVertex(OffsetRay(currentSurface.worldPosition, originNormal), connectionDir);

  if(!rcQuery.hit || rcQuery.instanceId != reservoir.rcVertexInstanceId || rcQuery.primitiveIndex != reservoir.rcVertexPrimitiveIndex)
  {
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeOccluded));
  }

  // BSDFs at both ends of the connection
  // Both ends evaluate the SAME proposal group the base path sampled, not the full mixture. The supplemental preserves the lobe index across reconnection, and it has to: SampleSurfaceBsdf divided by a joint per-lobe density, so reproducing its factorization requires dividing by that same per-lobe density.
  // Summing all groups and using the marginal density instead loses energy systematically.

  const uint prevLobeKind = (reservoir.pathFlags & RESTIR_PT_PATH_FLAGS_RC_PREV_LOBE_MASK) >> RESTIR_PT_PATH_FLAGS_RC_PREV_LOBE_SHIFT;
  const uint rcLobeKind   = (reservoir.pathFlags & RESTIR_PT_PATH_FLAGS_RC_LOBE_MASK) >> RESTIR_PT_PATH_FLAGS_RC_LOBE_SHIFT;

  float        prefixPdf  = 0.0;
  const float3 prefixBsdf = EvaluateSurfaceBsdfGroup(currentSurface, currentViewDir, connectionDir, prevLobeKind, prefixPdf);

  // The preserved lobe has no support for this direction in the target domain, so the shift has no valid counterpart here.
  if(!(prefixPdf > 0.0) || SafeMax3(prefixBsdf) <= 0.0)
  {
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomePrevLobeUnsupported));
  }

  // The last prefix vertex is a candidate like every other, and it is only testable here: the direction leaving it in this domain is the connection itself, which did not exist while the prefix was being walked.
  if(hasPredecessor && OffsetPathQualifiesForReconnection(previousSurface, previousLobeKind, previousSamplePdf, currentSurface, prevLobeKind, prefixPdf, destinationSurface))
  {
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReconnectionDisagreement));
  }

  float        rcPdf  = 0.0;
  const float3 rcBsdf = EvaluateSurfaceBsdfGroup(rcQuery.surface, -connectionDir, reservoir.rcVertexWi, rcLobeKind, rcPdf);

  if(!(rcPdf > 0.0) || SafeMax3(rcBsdf) <= 0.0)
  {
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeRcLobeUnsupported));
  }

  // The other half of the agreement: having qualified nowhere earlier, the offset path must qualify HERE, or it would have postponed its reconnection past this vertex and disagreed with the base path again.
  if(!OffsetPathQualifiesForReconnection(currentSurface, prevLobeKind, prefixPdf, rcQuery.surface, rcLobeKind, rcPdf, destinationSurface))
  {
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeReconnectionDisagreement));
  }

  // Throughput through the connection
  // These two bounces are roulette vertices in the base path just like the replayed ones, but they fall outside the loop above: the connection event at the predecessor takes the path to depth k, and the scattering event at the reconnection vertex takes it to depth k+1.
  // The stored suffix radiance was divided out AFTER both of those roulette divisions, so both must be reapplied here or the reconstructed throughput is short by exactly those factors.

  throughput *= prefixBsdf / prefixPdf;
  ApplyReplayRouletteDensity(throughput, reconnectionLength);

  throughput *= rcBsdf / rcPdf;
  ApplyReplayRouletteDensity(throughput, reconnectionLength + 1u);

  // Jacobian
  // Equation 2. D_y is the same three terms as the stored D_x, recomputed in the target domain; the Jacobian is their ratio. For a self-shift every term matches its base counterpart, so this is exactly 1.

  const float geometryTerm       = max(0.0, dot(currentSurface.geometricNormal, connectionDir)) / distanceSquared;
  const float shiftedDenominator = prefixPdf * geometryTerm * rcPdf;
  const float baseDenominator    = reservoir.rcVertexJacobianTerms;

  // A zero or non-finite denominator is a failed shift, never a large finite Jacobian: clamping one would inject an arbitrary weight into the estimator.
  if(!(baseDenominator > 0.0) || !(shiftedDenominator > 0.0))
  {
    return FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeBadDenominator));
  }

  // Result
  // A Jacobian far from one means the shift moved the path into a wildly different density, and such a sample carries a correspondingly wild contribution weight into reuse, where it is copied to neighbours. Refusing such a shift would be unbiased: a refused shift is a null candidate whose confidence still counts in the MIS weights.
  // There is no upper bound on the Jacobian here, deliberately. A symmetric sanity bound (refuse unless 1/k <= J <= k) was implemented and measured at k = 5 and 20: it does NOT stop the paired-reuse runaway. Area Light still reaches 2.98e33 by 1200 frames with k = 20, and Bunny Metallic diverges at every k tried.
  // A clamp suppresses the carrier without touching the correlated-confidence assumption that actually lets it replicate, and it would add bias of its own. Section 4's footprint criterion is the intended conditioning; see PAPER_ALIGNMENT.md.

  PTShiftResult result;

  result.integrand              = throughput * reservoir.rcVertexRadiance;
  result.jacobian               = shiftedDenominator / baseDenominator;
  result.destinationDenominator = shiftedDenominator;
  result.sourceDenominator      = baseDenominator;
  result.valid                  = isfinite(result.jacobian) && result.jacobian > 0.0;
  result.outcome                = result.valid ? uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeSuccess) : uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeBadDenominator);

  return result;
}

#endif // RESTIR_PT_SHIFT_H
