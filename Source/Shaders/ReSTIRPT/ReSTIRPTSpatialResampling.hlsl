// Spatial resampling
// ReSTIR PT Enhanced. Combines this pixel's reservoir with those of N nearby pixels in the SAME frame.
// Where temporal reuse pulls a path across time from the same surface point, spatial reuse pulls one across screen space from a different surface, so the shift genuinely relocates a path rather than merely re-anchoring it.
// Uses DEFENSIVE PAIRWISE MIS rather than the full generalized balance heuristic. The balance heuristic would need every candidate evaluated against every other technique, O(N^2) shifts.
// Pairing each neighbour only against the canonical costs 2N shifts (one forward, one inverse per neighbour) and still partitions unity, because the canonical carries the remainder. "Defensive" means the canonical also keeps a guaranteed baseline share c_C/C_T, protecting the one technique that is always valid.

#include <Common/ShaderTypes.h>
#include "ShaderIncludes/Random.hlsli"
#include "ShaderIncludes/Sky.hlsli"
#include "ShaderIncludes/Camera.hlsli"
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PTGlobals.hlsli"
#include "ShaderIncludes/ReSTIR/Common.hlsli"
#include "ReSTIR/PTReservoir.hlsli"

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"
#include "ShaderIncludes/PathTracing/Volume.hlsli"
#include "ShaderIncludes/PathTracing/Hdri.hlsli"
#include "ShaderIncludes/PathTracing/Disney.hlsli"
#include "ShaderIncludes/PathTracing/Intersection.hlsli"
#include "ShaderIncludes/PathTracing/Lights.hlsli"
#include "ReSTIR/PTReservoirStorage.hlsli"
#include "ReSTIR/PTShift.hlsli"
#include "ShaderIncludes/PathTracing/PathRayEntryPoints.hlsli"
#include "ShaderIncludes/ReSTIR/PTSpatialCommon.hlsli"

// Upper bound on neighbours per pixel, so the acceptance list can live in registers. The requested count is clamped to this.
static const uint kMaxSpatialNeighbours = 8;

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID   = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo  = pushConst.sceneInfoAddress.Get();
  const uint2         viewport   = (uint2)sceneInfo.viewportSize;
  const uint          pixelIndex = launchID.y * viewport.x + launchID.x;

  const uint2 reservoirPosition = PTPixelPosToReservoirPos(launchID);

  // Canonical sample
  // shadingWeight defaults to this pixel's own contribution (Section 6.3) so that every exit path from this shader leaves a usable value: a pixel that reuses nothing still has to be shaded.

  ReSTIRPTReservoir canonical = LoadPTReservoir(ptParams.reservoirBufferParams, reservoirPosition, ptParams.bufferIndices.spatialResamplingInputBufferIndex);

  float3 shadingWeight = canonical.F * canonical.ucw;

  const ReSTIRPTSurface currentSurfaceRecord = currentSurfaceBuffer[pixelIndex];

  // Destination surface
  // Forward shifts land on this pixel's primary hit, which needs a full material record; it is rebuilt from the hit identity initial sampling stored, so this pass no longer re-traces the primary ray.

  SurfaceData primarySurface;
  float3      primaryViewDir;

  const bool primaryValid = LoadPTSurfaceAtStoredHit(currentSurfaceRecord, sceneInfo.cameraPosition, primarySurface, primaryViewDir);

  if(primaryValid && IsValidPTReservoir(canonical))
  {
    // Acceptance pass
    // The accepted set and its confidence sum are fixed BEFORE any shift runs: the MIS weights divide by C_N, so letting a shift outcome change membership would make the technique set depend on the samples drawn, which carries no unbiasedness guarantee.

    int2  acceptedPixel[kMaxSpatialNeighbours];
    float acceptedConfidence[kMaxSpatialNeighbours];
    // Which pairing slot produced each neighbour, so the shared record can be located again. Unused on the unpaired path.
    uint  acceptedSlot[kMaxSpatialNeighbours];
    uint  acceptedCount          = 0;
    float confidenceNeighbourSum = 0.0;

    const bool usePairedReuse = ptParams.spatialResampling.enablePairedSpatialReuse != 0u;

    uint seed = PTVertexSeed(canonical.initRandomSeed, 0xA5A5u, ptParams.runtimeParams.uniformRandomNumber);

    // Paired reuse draws one neighbour per pairing texture, so it cannot ask for more neighbours than there are textures.
    const uint requestedCount = usePairedReuse ? min(ptParams.spatialResampling.numSamples, uint(RESTIR_PT_MAX_PAIRING_TEXTURES)) : min(ptParams.spatialResampling.numSamples, kMaxSpatialNeighbours);

    for(uint candidate = 0; candidate < requestedCount; ++candidate)
    {
      int2 neighbourPixel;

      if(usePairedReuse)
      {
        // The partner is dictated by the texture, not sampled. That is the whole point: a freely chosen neighbour could not be relied on to choose this pixel back, and without that reciprocity the shared shift is unusable.
        if(!PTFindPairedNeighbour(launchID, candidate, viewport, neighbourPixel))
        {
          continue;
        }
      }
      else
      {
        neighbourPixel = (int2)launchID + SampleNeighbourOffset(seed, ptParams.spatialResampling.samplingRadius);

        if(any(neighbourPixel < int2(0, 0)) || any(neighbourPixel >= (int2)viewport) || all(neighbourPixel == (int2)launchID))
        {
          continue;
        }
      }

      const uint            neighbourIndex   = uint(neighbourPixel.y) * viewport.x + uint(neighbourPixel.x);
      const ReSTIRPTSurface neighbourSurface = currentSurfaceBuffer[neighbourIndex];

      // Identical to the test the pre-pass applied, and it must stay identical: if the two passes disagreed, this pass would read a record for a pair the pre-pass declined to compute.
      if(!PTArePixelsCompatible(currentSurfaceRecord, neighbourSurface))
      {
        continue;
      }

      const ReSTIRPTReservoir probe = LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos((uint2)neighbourPixel), ptParams.bufferIndices.spatialResamplingInputBufferIndex);

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
      // No neighbour was accepted, so the canonical reservoir and its shading weight are stored unchanged.
    }
    else
    {
      // Confidence shares
      // Spatial confidence is NOT capped: the paper caps only temporal reuse, and sums the confidences of every accepted spatial neighbour.
      // The defensive baseline gives the canonical a guaranteed share C_C/C_total; this is what separates defensive pairwise MIS from plain pairwise.
      // The partition invariant is that the confidence SHARES sum to one: the canonical's C_C/C_total plus each pair's c_i/C_total. That is exact in real arithmetic, so any residual is accumulated rounding or a structural error in how the shares are formed.
      // It is NOT that the canonical and neighbour MIS weights sum to one: those are evaluated at DIFFERENT points in path space (pairCanonical at the canonical sample, pairNeighbour at the neighbour's), so they are not required to add to one.
      // shareSum accumulates the share sum for that check. The check itself was removed along with the pass diagnostics (an earlier version measured only whether misCanonical left [0,1], which misses every partition failure that stays in range), so nothing reads shareSum now.

      const float confidenceCanonical = canonical.M;
      const float confidenceTotal     = confidenceCanonical + confidenceNeighbourSum;
      const float targetCanonicalHere = canonical.targetPdf;

      float misCanonical = confidenceCanonical / confidenceTotal;
      float shareSum     = misCanonical;

      // Streaming state
      // Weighted reservoir sampling is order independent, so neighbours stream as they are evaluated and the canonical joins at the end, once its MIS weight has collected every pair's contribution.
      // vectorWeight (Section 6.3) accumulates alongside weightSum with RGB integrands in place of their luminances. Free here: every F it needs was already evaluated to form the scalar weights.
      // Selection randomness is drawn from the PIXEL, never from the candidate. Deriving it from a reservoir's own initRandomSeed makes the draw a deterministic function of the sample being selected, which RIS assumes it is not, and once reuse starts copying whole reservoirs between pixels, neighbours that inherited the same path also inherit the same draw, so the correlation spans the image rather than a single pixel.

      ReSTIRPTReservoir selected       = canonical;
      float             selectedTarget = targetCanonicalHere;
      float             weightSum      = 0.0;
      float3            vectorWeight   = (float3)0.0;
      uint              selectionSeed  = PTVertexSeed(XxHash32(uint3(launchID, ptParams.runtimeParams.frameIndex)), 0x5A5Au, 0u);
      bool              neighbourWon   = false;

      for(uint n = 0; n < acceptedCount; ++n)
      {
        const int2 neighbourPixel = acceptedPixel[n];
        const uint neighbourIndex = uint(neighbourPixel.y) * viewport.x + uint(neighbourPixel.x);

        // This pair's share of unity. Algebraically tau * alpha, that is (C_N/C_total) * (c_i/C_N), but formed in a single division: the C_N cancels exactly in exact arithmetic and only contributes rounding in floating point. Worth doing because the shares must sum to one and every rounding step widens that residual.
        const float pairShare = acceptedConfidence[n] / confidenceTotal;

        const ReSTIRPTSurface   neighbourSurface = currentSurfaceBuffer[neighbourIndex];
        const ReSTIRPTReservoir neighbour        = LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos((uint2)neighbourPixel), ptParams.bufferIndices.spatialResamplingInputBufferIndex);

        // Shifts
        // Forward maps the neighbour's path into this pixel's domain; inverse maps the canonical path into the neighbour's domain.

        PTShiftResult forward;
        PTShiftResult inverse;

        if(usePairedReuse)
        {
          // Both shifts already exist, one computed by each partner in the pre-pass. The neighbour's record IS the forward shift into this pixel, because reciprocity guarantees this pixel is that neighbour's partner in the same slot. Nothing is traced here.
          const uint slot = acceptedSlot[n];

          forward = PTPairedShiftToResult(ptPairedShiftBuffer[PTPairedShiftIndex((uint2)neighbourPixel, slot, viewport)]);
          inverse = PTPairedShiftToResult(ptPairedShiftBuffer[PTPairedShiftIndex(launchID, slot, viewport)]);
        }
        else
        {
          forward = ShiftPathToSurface(neighbour, primarySurface, primaryViewDir);

          SurfaceData neighbourPrimarySurface;
          float3      neighbourPrimaryViewDir;

          inverse = FailedPTShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoSource));

          // The neighbour's own primary hit, rebuilt from its stored identity rather than traced for, as this pixel's was.
          if(LoadPTSurfaceAtStoredHit(neighbourSurface, sceneInfo.cameraPosition, neighbourPrimarySurface, neighbourPrimaryViewDir))
          {
            inverse = ShiftPathToSurface(canonical, neighbourPrimarySurface, neighbourPrimaryViewDir);
          }
        }

        // Canonical share of this pair
        // Evaluated at the canonical sample: C_C pHat_C / (C_C pHat_C + C_N pHat_N(inverse) J_inverse), scaled by pairShare.
        // A failed inverse means this neighbour's technique cannot have produced the canonical path, so the canonical takes the pair's whole share. Handled direction-locally: the two shifts are evaluated at different points in path space, so one failing says nothing about the other.

        float pairCanonical = 1.0;

        if(inverse.valid)
        {
          const float targetCanonicalThere = ReSTIRLuminance(inverse.integrand);
          const float inverseDenominator   = confidenceCanonical * targetCanonicalHere + confidenceNeighbourSum * targetCanonicalThere * inverse.jacobian;

          pairCanonical = inverseDenominator > 0.0 ? (confidenceCanonical * targetCanonicalHere) / inverseDenominator : 1.0;
        }

        misCanonical += pairShare * pairCanonical;
        shareSum     += pairShare;

        // Neighbour candidate
        // Evaluated at the neighbour sample: C_N pHat_N / (C_N pHat_N + C_C pHat_C(forward) J_forward), scaled by pairShare. Its resampling weight is that MIS weight times pHat in this domain, the neighbour's ucw, and the forward Jacobian.
        // A failed forward shift makes this candidate null (zero weight), but its confidence stays in C_N, because the technique still ran.

        if(forward.valid)
        {
          const float targetNeighbourHere  = ReSTIRLuminance(forward.integrand);
          const float targetNeighbourThere = neighbour.targetPdf;
          const float forwardDenominator   = confidenceNeighbourSum * targetNeighbourThere + confidenceCanonical * targetNeighbourHere * forward.jacobian;
          const float pairNeighbour        = forwardDenominator > 0.0 ? (confidenceNeighbourSum * targetNeighbourThere) / forwardDenominator : 0.0;

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

              // Rebase Equation 2's base denominator into this pixel's domain, or a later shift divides by a domain this path no longer belongs to.
              selected.rcVertexJacobianTerms = forward.destinationDenominator;
              selectedTarget                 = targetNeighbourHere;
              neighbourWon                   = true;
            }
          }
        }
      }

      // Canonical candidate
      // Joins last, with the MIS weight that collected every pair's share. Its own term also closes the vector weight sum.

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

      vectorWeight += misCanonical * canonical.F * canonical.ucw;
      shadingWeight = vectorWeight;

      // Finalize
      // Every accepted neighbour contributes its confidence whether or not its forward shift produced a usable candidate: the MIS weights were built with it included. Clamped only because the packed field is 8 bits.

      selected.weightSum = weightSum;
      selected.targetPdf = selectedTarget;
      selected.M         = min(confidenceTotal, float(RESTIR_PT_PATH_FLAGS_M_MASK));
      selected.ucw       = selectedTarget > 0.0 ? weightSum / selectedTarget : 0.0;
      canonical          = selected;
    }
  }

  // Store
  // Written for every pixel, reused or not: final shading and the next frame read every element of both outputs.

  ptShadingWeightBuffer[pixelIndex] = shadingWeight;

  StorePTReservoir(canonical, ptParams.reservoirBufferParams, reservoirPosition, ptParams.bufferIndices.spatialResamplingOutputBufferIndex);
}

