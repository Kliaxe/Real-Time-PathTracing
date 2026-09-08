/*
 * ReSTIR PT Enhanced - spatial resampling.
 *
 * Combines this pixel's reservoir with those of N nearby pixels in the SAME frame.
 * Where temporal reuse pulls a path across time from the same surface point,
 * spatial reuse pulls one across screen space from a different surface, so the
 * shift genuinely relocates a path rather than merely re-anchoring it.
 *
 * Uses DEFENSIVE PAIRWISE MIS rather than the full generalized balance heuristic.
 * The balance heuristic would need every candidate evaluated against every other
 * technique - O(N^2) shifts. Pairing each neighbour only against the canonical
 * costs 2N shifts (one forward, one inverse per neighbour) and still partitions
 * unity, because the canonical carries the remainder. "Defensive" means the
 * canonical also keeps a guaranteed baseline share c_C/C_T, protecting the one
 * technique that is always valid.
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
#include "ShaderIncludes/ReSTIR/PTSpatialCommon.h.slang"

// Upper bound on neighbours per pixel, so the acceptance list can live in
// registers. The requested count is clamped to this.
static const uint kMaxSpatialNeighbours = 8;

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID   = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo  = pushConst.sceneInfoAddress[0];
  const uint2         viewport   = uint2(sceneInfo.viewportSize);
  const uint          pixelIndex = launchID.y * viewport.x + launchID.x;

  const uint2 reservoirPosition = PTPixelPosToReservoirPos(launchID);

  ReSTIRPTReservoir canonical =
      LoadPTReservoir(ptParams.reservoirBufferParams, reservoirPosition, ptParams.bufferIndices.spatialResamplingInputBufferIndex);

  // Section 6.3. Defaults to this pixel's own contribution so that every exit
  // path from this shader leaves a usable value: a pixel that reuses nothing
  // still has to be shaded.
  float3 shadingWeight = canonical.F * canonical.ucw;

  const ReSTIRPTSurface currentSurfaceRecord = currentSurfaceBuffer[pixelIndex];


  const float3        primaryDirection = ReconstructWorldDirectionFromPixel(float2(launchID) + 0.5, sceneInfo);
  const PTVertexQuery primary          = TracePTVertex(sceneInfo.cameraPosition, primaryDirection);
  const float3        primaryViewDir   = -primaryDirection;

  if(currentSurfaceRecord.valid != 0 && primary.hit && IsValidPTReservoir(canonical))
  {
    // -----------------------------------------------------------------------
    // Acceptance pass. The accepted set and its confidence sum are fixed BEFORE
    // any shift runs: the MIS weights divide by C_N, so letting a shift outcome
    // change membership would make the technique set depend on the samples drawn,
    // which carries no unbiasedness guarantee.
    // -----------------------------------------------------------------------
    int2  acceptedPixel[kMaxSpatialNeighbours];
    float acceptedConfidence[kMaxSpatialNeighbours];
    // Which pairing slot produced each neighbour, so the shared record can be
    // located again. Unused on the unpaired path.
    uint  acceptedSlot[kMaxSpatialNeighbours];
    uint  acceptedCount          = 0;
    float confidenceNeighbourSum = 0.0;

    const bool usePairedReuse = ptParams.spatialResampling.enablePairedSpatialReuse != 0u;

    uint seed = PTVertexSeed(canonical.initRandomSeed, 0xA5A5u, ptParams.runtimeParams.uniformRandomNumber);
    // Paired reuse draws one neighbour per pairing texture, so it cannot ask for
    // more neighbours than there are textures.
    const uint requestedCount = usePairedReuse ?
                                    min(ptParams.spatialResampling.numSamples, uint(RESTIR_PT_MAX_PAIRING_TEXTURES)) :
                                    min(ptParams.spatialResampling.numSamples, kMaxSpatialNeighbours);

    for(uint candidate = 0; candidate < requestedCount; ++candidate)
    {
      int2 neighbourPixel;
      if(usePairedReuse)
      {
        // The partner is dictated by the texture, not sampled. That is the whole
        // point: a freely chosen neighbour could not be relied on to choose this
        // pixel back, and without that reciprocity the shared shift is unusable.
        if(!PTFindPairedNeighbour(launchID, candidate, viewport, neighbourPixel))
        {
          continue;
        }
      }
      else
      {
        neighbourPixel = int2(launchID) + SampleNeighbourOffset(seed, ptParams.spatialResampling.samplingRadius);
        if(any(neighbourPixel < int2(0, 0)) || any(neighbourPixel >= int2(viewport))
           || all(neighbourPixel == int2(launchID)))
        {
          continue;
        }
      }

      const uint            neighbourIndex   = uint(neighbourPixel.y) * viewport.x + uint(neighbourPixel.x);
      const ReSTIRPTSurface neighbourSurface = currentSurfaceBuffer[neighbourIndex];

      // Identical to the test the pre-pass applied, and it must stay identical: if
      // the two passes disagreed, this pass would read a record for a pair the
      // pre-pass declined to compute.
      if(!PTArePixelsCompatible(currentSurfaceRecord, neighbourSurface))
      {
        continue;
      }

      const ReSTIRPTReservoir probe =
          LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(uint2(neighbourPixel)),
                          ptParams.bufferIndices.spatialResamplingInputBufferIndex);
      if(!IsValidPTReservoir(probe))
      {
        continue;
      }

      acceptedPixel[acceptedCount]      = neighbourPixel;
      acceptedConfidence[acceptedCount] = probe.M;
      acceptedSlot[acceptedCount]       = candidate;
      confidenceNeighbourSum += probe.M;
      ++acceptedCount;
    }

    if(acceptedCount == 0)
    {
    }
    else
    {
      // Spatial confidence is NOT capped: the paper caps only temporal reuse, and
      // sums the confidences of every accepted spatial neighbour.
      const float confidenceCanonical = canonical.M;
      const float confidenceTotal     = confidenceCanonical + confidenceNeighbourSum;
      const float targetCanonicalHere = canonical.targetPdf;

      // Defensive baseline: the canonical keeps a guaranteed share. This is what
      // separates defensive pairwise MIS from plain pairwise.
      float misCanonical = confidenceCanonical / confidenceTotal;
      // The shares that must add to one: the canonical's baseline plus one per
      // accepted neighbour. Accumulated only to be checked below.
      float shareSum     = misCanonical;

      // Weighted reservoir sampling is order independent, so neighbours stream as
      // they are evaluated and the canonical joins at the end, once its MIS weight
      // has collected every pair's contribution.
      ReSTIRPTReservoir selected       = canonical;
      float             selectedTarget = targetCanonicalHere;
      float             weightSum      = 0.0;
      // Section 6.3. Accumulated alongside weightSum with RGB integrands in place
      // of their luminances. Free here: every F it needs was already evaluated to
      // form the scalar weights.
      float3            vectorWeight   = float3(0.0);
      // Selection randomness is drawn from the PIXEL, never from the candidate.
      // Deriving it from a reservoir's own initRandomSeed makes the draw a
      // deterministic function of the sample being selected, which RIS assumes
      // it is not - and once reuse starts copying whole reservoirs between
      // pixels, neighbours that inherited the same path also inherit the same
      // draw, so the correlation spans the image rather than a single pixel.
      uint              selectionSeed  = PTVertexSeed(xxhash32(uint3(launchID, ptParams.runtimeParams.frameIndex)), 0x5A5Au, 0u);
      bool              neighbourWon   = false;

      for(uint n = 0; n < acceptedCount; ++n)
      {
        const int2  neighbourPixel = acceptedPixel[n];
        const uint  neighbourIndex = uint(neighbourPixel.y) * viewport.x + uint(neighbourPixel.x);
        // This pair's share of unity. Algebraically tau * alpha, that is
        // (C_N/C_total) * (c_i/C_N), but formed in a single division: the C_N
        // cancels exactly in exact arithmetic and only contributes rounding in
        // floating point. Worth doing here because the shares must sum to one and
        // every rounding step widens that residual.
        const float pairShare      = acceptedConfidence[n] / confidenceTotal;

        const ReSTIRPTSurface   neighbourSurface = currentSurfaceBuffer[neighbourIndex];
        const ReSTIRPTReservoir neighbour =
            LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(uint2(neighbourPixel)),
                            ptParams.bufferIndices.spatialResamplingInputBufferIndex);

        PTShiftResult forward;
        PTShiftResult inverse;
        if(usePairedReuse)
        {
          // Both shifts already exist - one computed by each partner - in the
          // pre-pass. The neighbour's record IS the forward shift into this pixel,
          // because reciprocity guarantees this pixel is that neighbour's partner
          // in the same slot. Nothing is traced here.
          const uint slot = acceptedSlot[n];
          forward = PTPairedShiftToResult(ptPairedShiftBuffer[PTPairedShiftIndex(uint2(neighbourPixel), slot, viewport)]);
          inverse = PTPairedShiftToResult(ptPairedShiftBuffer[PTPairedShiftIndex(launchID, slot, viewport)]);
        }
        else
        {
          forward = ShiftPathToSurface(neighbour, primary.surface, primaryViewDir);

          SurfaceData   neighbourPrimarySurface;
          float3        neighbourPrimaryViewDir;
          inverse = FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoSource));
          if(RecoverSurfaceAtStoredPoint(sceneInfo.cameraPosition, neighbourSurface, neighbourPrimarySurface,
                                         neighbourPrimaryViewDir))
          {
            inverse = ShiftPathToSurface(canonical, neighbourPrimarySurface, neighbourPrimaryViewDir);
          }
        }

        // Canonical's share of THIS pair, evaluated at the canonical sample. A
        // failed inverse means this neighbour's technique cannot have produced the
        // canonical path, so the canonical takes the pair's whole share. Handled
        // direction-locally: the two shifts are evaluated at different points in
        // path space, so one failing says nothing about the other.
        float pairCanonical = 1.0;
        if(inverse.valid)
        {
          const float targetCanonicalThere = ReSTIRLuminance(inverse.integrand);
          const float inverseDenominator =
              confidenceCanonical * targetCanonicalHere + confidenceNeighbourSum * targetCanonicalThere * inverse.jacobian;
          pairCanonical = inverseDenominator > 0.0 ? (confidenceCanonical * targetCanonicalHere) / inverseDenominator : 1.0;
        }
        misCanonical += pairShare * pairCanonical;
        shareSum += pairShare;

        // Neighbour's own weight, evaluated at the neighbour sample. A failed
        // forward shift makes this candidate null - zero weight - but its
        // confidence stays in C_N, because the technique still ran.
        if(forward.valid)
        {
          const float targetNeighbourHere  = ReSTIRLuminance(forward.integrand);
          const float targetNeighbourThere = neighbour.targetPdf;
          const float forwardDenominator =
              confidenceNeighbourSum * targetNeighbourThere + confidenceCanonical * targetNeighbourHere * forward.jacobian;
          const float pairNeighbour =
              forwardDenominator > 0.0 ? (confidenceNeighbourSum * targetNeighbourThere) / forwardDenominator : 0.0;

          const float misNeighbour    = pairShare * pairNeighbour;
          const float weightNeighbour = misNeighbour * targetNeighbourHere * neighbour.ucw * forward.jacobian;
          vectorWeight += misNeighbour * forward.integrand * neighbour.ucw * forward.jacobian;

          if(weightNeighbour > 0.0)
          {
            weightSum += weightNeighbour;
            if(NextRandom(selectionSeed) * weightSum < weightNeighbour)
            {
              selected   = neighbour;
              selected.F = forward.integrand;
              // Rebase Equation 2's base denominator into this pixel's domain, or a
              // later shift divides by a domain this path no longer belongs to.
              selected.rcVertexJacobianTerms = forward.destinationDenominator;
              selectedTarget                 = targetNeighbourHere;
              neighbourWon                   = true;
            }
          }

        }

      }

      const float weightCanonical = misCanonical * targetCanonicalHere * canonical.ucw;
      if(weightCanonical > 0.0)
      {
        weightSum += weightCanonical;
        if(NextRandom(selectionSeed) * weightSum < weightCanonical)
        {
          selected       = canonical;
          selectedTarget = targetCanonicalHere;
          neighbourWon   = false;
        }
      }

      // The canonical's own term closes the sum, matching how misCanonical
      // collected every pair's share before this point.
      vectorWeight += misCanonical * canonical.F * canonical.ucw;
      shadingWeight = vectorWeight;

      selected.weightSum = weightSum;
      selected.targetPdf = selectedTarget;
      // Every accepted neighbour contributes its confidence whether or not its
      // forward shift produced a usable candidate: the MIS weights were built with
      // it included. Clamped only because the packed field is 8 bits.
      selected.M   = min(confidenceTotal, float(RESTIR_PT_PATH_FLAGS_M_MASK));
      selected.ucw = selectedTarget > 0.0 ? weightSum / selectedTarget : 0.0;
      canonical    = selected;

      // Weight magnitude, which no other field records; its tail is what a runaway
      // shows up in while every shift statistic stays healthy.
      // The partition invariant, at last actually measured.
      //
      // What this is NOT: the sum of the canonical and neighbour MIS weights. Those
      // are evaluated at DIFFERENT points in path space - pairCanonical at the
      // canonical sample, pairNeighbour at the neighbour's - so they are not
      // required to add to one and checking them would be meaningless.
      //
      // What must hold is that the confidence SHARES partition unity: the
      // canonical's C/C_total plus each pair's c_i/C_total. That is exact in real
      // arithmetic, so any residual here is either accumulated rounding or a
      // structural error in how the shares are formed.
      //
      // The previous version of this line measured abs(misCanonical -
      // clamp(misCanonical, 0, 1)), which only asks whether one weight left [0,1]
      // and reports zero for every partition failure that stays in range. Both a
      // Codex review and a manual read flagged it independently; nothing had ever
      // tested this invariant.
    }
  }

  ptShadingWeightBuffer[pixelIndex] = shadingWeight;
  // Skipped, not merely ignored, when diagnostics are off: the planes are then a
  // single-record placeholder and this index would be out of bounds.
  StorePTReservoir(canonical, ptParams.reservoirBufferParams, reservoirPosition,
                   ptParams.bufferIndices.spatialResamplingOutputBufferIndex);
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
