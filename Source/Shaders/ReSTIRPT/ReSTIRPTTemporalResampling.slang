/*
 * ReSTIR PT Enhanced - temporal resampling.
 *
 * Combines this frame's initial candidate with the reservoir the reprojected pixel
 * held last frame. This is the first pass where source and destination pixels
 * genuinely differ, so it is the first real exercise of the hybrid shift: until
 * now the shift only ever mapped a path onto its own pixel.
 *
 * Structure follows GRIS (paper Section 2.1): each contributing sample is shifted
 * into the resampling domain, weighted by its MIS weight, its target function, and
 * its unbiased contribution weight, and the survivor's UCW is rebuilt from the
 * accumulated weight sum.
 */

#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "nvshaders/sky_functions.h.slang"
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PTGlobals.h.slang"
#include "ShaderIncludes/ReSTIR/Common.h.slang"
#include "ReSTIR/PTReservoir.hlsli"

#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"
#include "ReSTIR/PTReservoirStorage.hlsli"
#include "ReSTIR/PTShift.h.slang"

// Recovers the full material record for a stored surface.
//
// The stored ReSTIRPTSurface is deliberately reduced and cannot drive a BSDF, and
// LoadSurfaceData needs hit-shader intrinsics, so the only way to get a full
// surface for an arbitrary stored point is to trace at it. Aiming from the camera
// that saw it keeps the incident direction - and therefore the face-forwarding
// LoadSurfaceData applies - consistent with how the point was originally shaded.
bool RecoverSurfaceAtStoredPoint(float3 cameraPosition, ReSTIRPTSurface stored, out SurfaceData surface, out float3 viewDir)
{
  surface = (SurfaceData)0;
  viewDir = float3(0.0, 1.0, 0.0);
  if(stored.valid == 0)
  {
    return false;
  }

  const float3 toSurface = stored.worldPosition - cameraPosition;
  const float  distance  = length(toSurface);
  if(distance <= 1.0e-6)
  {
    return false;
  }

  const float3 direction = toSurface / distance;
  const PTVertexQuery query = TracePTVertex(cameraPosition, direction);
  if(!query.hit)
  {
    return false;
  }

  // Guard against landing on nearer geometry: the stored point must still be the
  // first thing the ray meets, or this is a different surface entirely.
  if(length(query.surface.worldPosition - stored.worldPosition) > 0.01 * distance)
  {
    return false;
  }

  surface = query.surface;
  viewDir = -direction;
  return true;
}

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID   = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo  = pushConst.sceneInfoAddress[0];
  const uint2         viewport   = uint2(sceneInfo.viewportSize);
  const uint          pixelIndex = launchID.y * viewport.x + launchID.x;

  const uint2 reservoirPosition = PTPixelPosToReservoirPos(launchID);

  // This frame's candidate. The temporal pass reads it, combines, and writes back
  // over the same array, which is why the candidate must be loaded before anything
  // is stored.
  ReSTIRPTReservoir canonical =
      LoadPTReservoir(ptParams.reservoirBufferParams, reservoirPosition, ptParams.bufferIndices.initialSamplingOutputBufferIndex);

  // Section 6.3. Defaults to this pixel's own contribution so that every exit
  // path from this shader leaves a usable value: a pixel that reuses nothing
  // still has to be shaded.
  float3 shadingWeight = canonical.F * canonical.ucw;

  const ReSTIRPTSurface currentSurfaceRecord = currentSurfaceBuffer[pixelIndex];

  // Endpoint depth of this pixel's own candidate, which is what explains a missing

  // Both shift directions need a full material record at their destination primary
  // hit, which only a closest-hit shader can produce. Re-tracing the primary ray is
  // how this pass obtains one for the current pixel; the stored surface record is
  // deliberately reduced and cannot drive a BSDF.
  const float3        primaryDirection = ReconstructWorldDirectionFromPixel(float2(launchID) + 0.5, sceneInfo);
  const PTVertexQuery primary          = TracePTVertex(sceneInfo.cameraPosition, primaryDirection);
  const float3        primaryViewDir   = -primaryDirection;

  // Frame zero has no history to reuse, and a background pixel has no surface to
  // reuse onto.
  const bool historyAvailable =
      ptParams.runtimeParams.frameIndex > 0 && currentSurfaceRecord.valid != 0 && primary.hit;

  // Section 6.4. The motion this pixel recorded last frame describes whatever was
  // visible here THEN - the occluder. Read before the write below, which is safe
  // because an invocation only ever touches its own element.
  const float2 occluderMotion = ptMotionVectorBuffer[pixelIndex];
  float2       currentMotion  = float2(0.0);

  if(historyAvailable)
  {
    // Reproject this pixel's primary hit through the previous camera. Every failure
    // below records a distinct outcome, so a low reuse rate is attributable rather
    // than merely visible.
    float3       motionVector;
    const float2 pixelCenter = float2(launchID) + 0.5;
    if(!TryComputeReSTIRTemporalMotionVector(pixelCenter, currentSurfaceRecord.worldPosition,
                                             currentSurfaceRecord.linearDepth, sceneInfo, motionVector))
    {
    }
    else
    {
      const float3 pixelMotion = ConvertMotionVectorToPixelSpace(pixelCenter, motionVector, sceneInfo);
      currentMotion            = pixelMotion.xy;

      // Section 6.4. A disoccluded pixel has no history of its own by definition:
      // its own motion vector points where nothing was showing it. The occluder's
      // motion is the second guess - a surface revealed by something moving away
      // usually shares that motion - and it costs one extra lookup on exactly the
      // pixels that would otherwise have started from scratch.
      float2 candidateMotion[2] = {currentMotion, occluderMotion};
      const uint candidateCount =
          (ptParams.temporalResampling.enableDualMotionVectors != 0u && any(occluderMotion != float2(0.0))) ? 2u : 1u;

      int2            previousPixel   = int2(0, 0);
      uint            previousIndex   = 0u;
      ReSTIRPTSurface previousSurface = (ReSTIRPTSurface)0;
      bool            surfaceMatches  = false;
      uint            failureOutcome  = uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeHistoryOutOfBounds);

      for(uint candidate = 0; candidate < candidateCount && !surfaceMatches; ++candidate)
      {
        const int2 candidatePixel = int2(floor(pixelCenter + candidateMotion[candidate]));
        if(any(candidatePixel < int2(0, 0)) || any(candidatePixel >= int2(viewport)))
        {
          failureOutcome = uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeHistoryOutOfBounds);
          continue;
        }

        const uint            candidateIndex   = uint(candidatePixel.y) * viewport.x + uint(candidatePixel.x);
        const ReSTIRPTSurface candidateSurface = previousSurfaceBuffer[candidateIndex];

        // Surface similarity gate: a cheap rejection before paying for a shift, and
        // the only defence against reusing a path that belonged to other geometry.
        // The dual candidate is held to exactly this test, so a wrong guess about
        // shared motion is rejected rather than reused.
        if(candidateSurface.valid == 0
           || !IsValidNeighbor(currentSurfaceRecord.shadingNormal, candidateSurface.shadingNormal,
                               currentSurfaceRecord.linearDepth, candidateSurface.linearDepth,
                               ptParams.temporalResampling.normalThreshold,
                               ptParams.temporalResampling.depthThreshold))
        {
          failureOutcome = uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeSurfaceMismatch);
          continue;
        }

        previousPixel   = candidatePixel;
        previousIndex   = candidateIndex;
        previousSurface = candidateSurface;
        surfaceMatches  = true;
      }

      {
        if(!surfaceMatches)
        {
        }
        else
        {
          const ReSTIRPTReservoir temporal =
              LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(uint2(previousPixel)),
                              ptParams.bufferIndices.temporalResamplingInputBufferIndex);

          if(!IsValidPTReservoir(temporal))
          {
          }
          else
          {
            // ---------------------------------------------------------------
            // GRIS combination (paper Equation 1) of two candidates: this
            // frame's canonical sample and the reprojected history sample.

            // Confidence weights. The canonical technique ran exactly once, so its
            // confidence is 1 even if its reservoir holds no positive sample: M=0
            // there means "no sample selected", not "the technique did not run".
            // Dropping it from the history candidate's denominator would remove the
            // canonical technique's coverage and overweight history.
            const float confidenceCanonical = 1.0;
            // The cap is applied HERE, before either MIS weight is computed, because
            // the same confidence appears in both denominators. Capping only the
            // stored result afterwards would leave the two weights disagreeing and
            // break their partition of unity.
            //
            // Section 5 makes the cap adaptive. The duplication score measured at
            // the END of the previous frame is read at the history pixel, because
            // that is the reservoir whose confidence is in question - reading the
            // score at the current pixel would ask about correlation that this
            // frame's reuse has not produced yet.
            //
            // score^gamma with gamma << 1 is a deliberately abrupt ramp: it treats
            // "any duplication at all" as the signal, since a handful of shared
            // seeds in a 17x17 window already means a firefly has a foothold and is
            // about to be amplified by further reuse.
            //
            // This is the one knowingly biased step in the renderer. Cap now depends
            // on WHICH sample the history holds, so the MIS weights no longer form a
            // partition of unity over the resampling domain and energy is lost where
            // the cap is cut. The bias is confined to already-correlated regions -
            // where the cap is untouched, the estimator is exactly as unbiased as
            // before - and the paper measures 3.25% mean absolute relative bias in a
            // scene chosen to be hostile. It buys a large reduction in the blobs and
            // streaks that no unbiased amount of reuse tuning removes.
            float historyCap = float(ptParams.temporalResampling.maxHistoryLength);
            if(ptParams.decorrelation.enable != 0u)
            {
              const float duplicationScore = saturate(ptDuplicationBuffer[previousIndex]);
              const float capBlend         = pow(duplicationScore, max(ptParams.decorrelation.gamma, 1e-4));
              // capMin is clamped to the base cap because the two are independent
              // controls: a capMin above maxHistoryLength would turn this lerp into
              // a cap INCREASE as duplication rises, amplifying exactly the
              // correlation it exists to suppress. Clamping here rather than in the
              // UI keeps the guarantee for every path that sets these values.
              const float capMin = min(ptParams.decorrelation.capMin, historyCap);
              historyCap         = lerp(historyCap, capMin, capBlend);
            }
            const float confidenceTemporal = min(temporal.M, historyCap);

            // Forward: history into this pixel's domain. Its integrand is what gets
            // stored if history wins.
            const PTShiftResult forward = ShiftPathToSurface(temporal, primary.surface, primaryViewDir);

            // Inverse: this pixel's canonical path into the history pixel's domain.
            // Required, not optional - the canonical candidate's MIS weight needs
            // the history technique's target value for the canonical path, and no
            // amount of forward shifting reveals that.
            SurfaceData previousPrimarySurface;
            float3      previousPrimaryViewDir;
            PTShiftResult inverse = FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoSource));
            if(RecoverSurfaceAtStoredPoint(sceneInfo.prevCameraPosition, previousSurface, previousPrimarySurface,
                                           previousPrimaryViewDir))
            {
              inverse = ShiftPathToSurface(canonical, previousPrimarySurface, previousPrimaryViewDir);
            }

            // Reuse only when the mapping is verified in BOTH directions.
            //
            // GRIS assumes a bijective shift, and requiring both directions is what
            // enforces that here: reuse is forgone wherever the mapping turns out to
            // be one-sided.
            //
            // That is conservative for THIS pass but not free overall. A refused
            // shift still lets the other technique's coverage down-weight the
            // canonical sample while contributing nothing itself, and once temporal
            // and spatial reuse are chained the refusals compound into measurable
            // energy gain - +86% on Cornell Box and +75% on Disocclusion Pillars
            // against the plain path-traced reference. Full random replay closes the
            // anchorless case and brings both back to the reference, which is why it
            // is on by default rather than an optional extra.
            const bool reuseIsBijective = forward.valid && inverse.valid;

            // Target function values. p-hat is the luminance of the integrand,
            // always evaluated in the domain named by the subscript.
            // The canonical reservoir is overwritten below by the combined result,
            // so its own integrand is captured first for the vector weight.
            const ReSTIRPTReservoir canonical0 = canonical;
            const float targetCanonicalHere  = canonical.targetPdf;
            const float targetTemporalThere  = temporal.targetPdf;
            const float targetTemporalHere   = reuseIsBijective ? ReSTIRLuminance(forward.integrand) : 0.0;
            const float targetCanonicalThere = reuseIsBijective ? ReSTIRLuminance(inverse.integrand) : 0.0;

            // Generalized balance heuristic. The Jacobians stay OUT of the target
            // values and multiply as separate factors, matching Equation 1.
            float misCanonical = 1.0;
            float misTemporal  = 0.0;
            if(reuseIsBijective)
            {
              const float canonicalDenominator =
                  confidenceCanonical * targetCanonicalHere + confidenceTemporal * targetCanonicalThere * inverse.jacobian;
              const float temporalDenominator =
                  confidenceTemporal * targetTemporalThere + confidenceCanonical * targetTemporalHere * forward.jacobian;

              misCanonical = canonicalDenominator > 0.0 ? (confidenceCanonical * targetCanonicalHere) / canonicalDenominator : 1.0;
              misTemporal  = temporalDenominator > 0.0 ? (confidenceTemporal * targetTemporalThere) / temporalDenominator : 0.0;
            }

            // Resampling weights, both expressed in this pixel's domain.
            const float weightCanonical = misCanonical * targetCanonicalHere * canonical.ucw;
            const float weightTemporal =
                reuseIsBijective ? misTemporal * targetTemporalHere * temporal.ucw * forward.jacobian : 0.0;
            const float weightSum = weightCanonical + weightTemporal;

            // Select proportionally to the resampling weights.
            // Selection randomness is drawn from the PIXEL, never from the candidate.
            // Deriving it from a reservoir's own initRandomSeed makes the draw a
            // deterministic function of the sample being selected, which RIS assumes
            // it is not - and once reuse starts copying whole reservoirs between
            // pixels, neighbours that inherited the same path also inherit the same
            // draw, so the correlation spans the image rather than a single pixel.
            uint  selectionSeed = PTVertexSeed(xxhash32(uint3(launchID, ptParams.runtimeParams.frameIndex)), 0xFFFFu, 0u);
            const bool historyWins =
                weightSum > 0.0 && (NextRandom(selectionSeed) * weightSum) >= weightCanonical;

            ReSTIRPTReservoir combined = canonical;
            if(historyWins)
            {
              // The surviving path now lives in this pixel's domain, so everything
              // describing it must be rebased here. The Jacobian is deliberately NOT
              // folded into F: Equation 1 multiplies it once in the resampling
              // weight, and baking it in would apply it twice on the next reuse.
              combined                       = temporal;
              combined.F                     = forward.integrand;
              combined.targetPdf             = targetTemporalHere;
              // Rebase Equation 2's base denominator to the destination domain.
              // Without this the next frame divides by the denominator of a domain
              // this path no longer belongs to, and the error compounds every frame.
              combined.rcVertexJacobianTerms = forward.destinationDenominator;
            }

            combined.weightSum = weightSum;
            // Must match the confidence the MIS weights were actually built from.
            // Claiming history confidence on a frame that reused nothing would let a
            // rejected pixel keep inflating its weight in later frames.
            combined.M = reuseIsBijective ? (confidenceCanonical + confidenceTemporal) : confidenceCanonical;
            combined.ucw       = combined.targetPdf > 0.0 ? weightSum / combined.targetPdf : 0.0;
            canonical          = combined;

            // Section 6.3. The same sum as weightSum, but carrying RGB integrands
            // instead of their luminances. It is the expectation of the scalar
            // estimator over which candidate the selection happened to pick, so it
            // has the same mean and no higher variance, and it importance-samples
            // chroma that a luminance target function ignores.
            shadingWeight = misCanonical * canonical0.F * canonical0.ucw;
            if(reuseIsBijective)
            {
              shadingWeight += misTemporal * forward.integrand * temporal.ucw * forward.jacobian;
            }

            // Only meaningful when both directions succeeded: with a one-sided
            // mapping the pair genuinely does not partition unity, and flagging that
            // as an error would drown the real signal.
            // Weight magnitude, which no other field records; its tail is what a runaway
            // shows up in while every shift statistic stays healthy.
          }
        }
      }
    }
  }

  // Section 6.4. Published for the next frame, where this pixel's occupant becomes
  // the occluder of whatever is revealed behind it.
  ptMotionVectorBuffer[pixelIndex]  = currentMotion;
  ptShadingWeightBuffer[pixelIndex] = shadingWeight;
  // Skipped, not merely ignored, when diagnostics are off: the planes are then a
  // single-record placeholder and this index would be out of bounds.
  StorePTReservoir(canonical, ptParams.reservoirBufferParams, reservoirPosition,
                   ptParams.bufferIndices.temporalResamplingOutputBufferIndex);
}

[shader("miss")]
void rmissMain(inout PTShiftPayload payload)
{
  payload.hit = 0u;
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload)
{
  payload.visible = 1;
}

[shader("anyhit")]
void rahitMain(inout PTShiftPayload payload, in BuiltInTriangleIntersectionAttributes attr)
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
void rchitMain(inout PTShiftPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  payload.surface        = LoadSurfaceData(attr);
  payload.hit            = 1u;
  payload.instanceId     = InstanceIndex();
  payload.primitiveIndex = PrimitiveIndex();
}
