/*
 * ReSTIR PT Enhanced - Section 6.2.2, pass 1 of 2: build the work list.
 *
 * Visits every (pixel, pairing slot) pair once. Pairs that need a shift append
 * themselves to a flat work list; pairs that do not get their paired-shift record
 * written here, with the outcome describing why.
 *
 * Writing the rejections here is required, not an optimization: the resampling
 * pass reads a partner's record without knowing whether that partner ran, so a
 * record left untouched would be read as a live shift from an earlier frame. The
 * traced pass only visits survivors, so this is the only pass that can write them.
 *
 * The cheap rejections are all decided here. Recovering the partner's surface is
 * not, even though it can also fail - it costs a ray, and doing it here as well as
 * in the traced pass would pay that twice for every surviving pair.
 *
 * WHY THERE IS NO SORT. This originally bucketed the list by divergence class so a
 * warp of cheap lanes would not wait on an expensive one, and the sort was blamed
 * for a divergence that turned out to have a different cause entirely (a mistuned
 * Section 5 alpha - see ReSTIRPTSettings.h). The sort is still gone, but for a
 * simpler reason than the one recorded here for a long time: the paper does not ask
 * for it. Section 6.2.2 is "Stream Compaction for Random Replay", and compaction is
 * what this pass does.
 *
 * One difference from the paper worth knowing: it compacts to pairs that REQUIRE
 * RANDOM REPLAY, while this compacts to pairs that pass the surface-compatibility
 * test. The paper's criterion is narrower and would launch fewer pairs; whether that
 * is worth the extra classification work here has not been measured.
 *
 * What compaction alone is worth, as the whole-frame GPU cost of adding this pass
 * (measured as the whole-frame cost with and without this pass):
 *
 *   Cornell Box      40.92 -> 21.72 ms   1.9x
 *   Disoccl. Pillars 30.67 -> 15.88 ms   1.9x
 *   Scattered Lights 48.08 -> 25.14 ms   1.9x
 *   Sponza Studio    13.14 ->  3.42 ms   3.8x
 *
 * The scene with real geometric and depth complexity gains twice what the closed
 * boxes do, which is the expected shape: divergence is what this removes, and a
 * Cornell box has far less of it to remove than a Sponza does. Measuring this only
 * on the small scenes would have understated it by half.
 *
 * Most of the speedup survives anyway, because it never came from the ordering:
 * the pass this replaces looped all three slots inside ONE invocation, so a warp
 * cost the SUM over slots of that slot's slowest lane - not three times one lane.
 * Parallelizing over pairs is what Section 6.2.2 actually asks for, and it also
 * stops the canonical reservoir and surface being reloaded once per active slot.
 */

#include <nvshaders/slang_types.h>
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PTGlobals.h.slang"
#include "ReSTIR/PTReservoir.hlsli"
#include "ReSTIR/PTReservoirStorage.hlsli"
#include "ReSTIR/PTPairing.h.slang"
#include "ReSTIR/PTPrepassSort.h.slang"

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 threadID: SV_DispatchThreadID)
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const uint2         viewport  = uint2(sceneInfo.viewportSize);
  const uint2         launchID  = threadID.xy;
  if(launchID.x >= viewport.x || launchID.y >= viewport.y)
  {
    return;
  }

  const uint pixelIndex = launchID.y * viewport.x + launchID.x;
  const uint slotCount  = min(ptParams.spatialResampling.numSamples, uint(RESTIR_PT_MAX_PAIRING_TEXTURES));

  const ReSTIRPTSurface   currentSurfaceRecord = currentSurfaceBuffer[pixelIndex];
  const ReSTIRPTReservoir canonical =
      LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(launchID),
                      ptParams.bufferIndices.spatialResamplingInputBufferIndex);
  const bool pixelUsable = currentSurfaceRecord.valid != 0 && IsValidPTReservoir(canonical);

  for(uint slot = 0; slot < RESTIR_PT_MAX_PAIRING_TEXTURES; ++slot)
  {
    const uint entry     = PTPairedShiftIndex(launchID, slot, viewport);
    uint       rejection = uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoSource);
    bool       accepted  = false;

    if(slot < slotCount && pixelUsable)
    {
      int2 partnerPixel;
      if(PTFindPairedNeighbour(launchID, slot, viewport, partnerPixel))
      {
        const uint            partnerIndex   = uint(partnerPixel.y) * viewport.x + uint(partnerPixel.x);
        const ReSTIRPTSurface partnerSurface = currentSurfaceBuffer[partnerIndex];
        accepted                             = PTArePixelsCompatible(currentSurfaceRecord, partnerSurface);
        if(!accepted)
        {
          rejection = uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeSurfaceMismatch);
        }
      }
    }

    if(accepted)
    {
      uint destination;
      InterlockedAdd(ptPrepassCounterBuffer[RESTIR_PT_PREPASS_COUNT_OFFSET], 1u, destination);
      ptPrepassWorkBuffer[destination] = PackPrepassWorkItem(pixelIndex, slot);
    }
    else
    {
      ptPairedShiftBuffer[entry] = MakeEmptyPTPairedShift(rejection);
    }
  }
}
