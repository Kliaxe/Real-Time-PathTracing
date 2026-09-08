/*
 * ReSTIR PT Enhanced - spatial reuse pre-pass (Section 3).
 *
 * Writes, per pixel and per paired neighbour slot, the shift of this pixel's path
 * into its partner's domain.
 *
 * Contract with the resampling pass: pairing is reciprocal, so a pair's two shifts
 * are the same two computations both partners need. Each pixel computes one; the
 * resampling pass then reads its own record as the inverse shift and its partner's
 * as the forward shift, tracing neither. Two shifts per pair instead of four.
 *
 * A pixel cannot read a record the same dispatch is still writing, which is what
 * forces the split into two passes and the barrier between them.
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
#include "ReSTIR/PTPrepassSort.h.slang"

// One (pixel, slot) pair: recover the partner's surface and shift this pixel's
// path into it. Shared by both dispatch shapes below so they cannot drift apart.
//
// The partner surface recovery stays here rather than in the classify pass even
// though it can fail: it costs a ray, and doing it in both would pay that for
// every surviving pair twice. Measured at 0.35 ms of a 19 ms pass, so it is not
// worth hoisting.
void ProcessPrepassPair(uint2 launchID, uint slot, uint2 viewport, GltfSceneInfo sceneInfo)
{
  const uint pixelIndex = launchID.y * viewport.x + launchID.x;
  const ReSTIRPTSurface currentSurfaceRecord = currentSurfaceBuffer[pixelIndex];
  const ReSTIRPTReservoir canonical =
      LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(launchID),
                      ptParams.bufferIndices.spatialResamplingInputBufferIndex);

  ReSTIRPTPairedShift record = MakeEmptyPTPairedShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoSource));

  int2 partnerPixel;
  if(currentSurfaceRecord.valid != 0 && IsValidPTReservoir(canonical)
     && PTFindPairedNeighbour(launchID, slot, viewport, partnerPixel))
  {
    const uint            partnerIndex   = uint(partnerPixel.y) * viewport.x + uint(partnerPixel.x);
    const ReSTIRPTSurface partnerSurface = currentSurfaceBuffer[partnerIndex];

    if(PTArePixelsCompatible(currentSurfaceRecord, partnerSurface))
    {
      SurfaceData partnerPrimarySurface;
      float3      partnerPrimaryViewDir;
      if(RecoverSurfaceAtStoredPoint(sceneInfo.cameraPosition, partnerSurface, partnerPrimarySurface, partnerPrimaryViewDir))
      {
        record = MakePTPairedShift(ShiftPathToSurface(canonical, partnerPrimarySurface, partnerPrimaryViewDir));
      }
      else
      {
        record = MakeEmptyPTPairedShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeOccluded));
      }
    }
    else
    {
      record = MakeEmptyPTPairedShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeSurfaceMismatch));
    }
  }

  ptPairedShiftBuffer[PTPairedShiftIndex(launchID, slot, viewport)] = record;
}

[shader("raygeneration")]
void rgenMain()
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const uint2         viewport  = uint2(sceneInfo.viewportSize);

  // Section 6.2.2. Sorted: one invocation per surviving pair, launched indirectly
  // over a work list already grouped by divergence class, so a warp's lanes do
  // comparable amounts of work. The branch is on a push constant, so it is uniform
  // across the dispatch and costs nothing.
  if((pushConst.flags & uint(ReSTIRPTFlags::eReSTIRPTFlagSortedPrepass)) != 0u)
  {
    const uint index = DispatchRaysIndex().x;
    // Strictly redundant: a ray dispatch launches exactly the requested dimensions,
    // with no rounded-up workgroup tail to guard against. Kept as a cheap assertion
    // that the launch size and the list agree, since a mismatch between them would
    // otherwise read unwritten entries silently.
    if(index >= ptPrepassCounterBuffer[RESTIR_PT_PREPASS_INDIRECT_OFFSET])
    {
      return;
    }

    // Bounds check on the unpacked pixel index. Measured as never triggering - an
    // instrumented run found 0 of 797223 entries out of range - so this is an
    // assertion, not a fix. It is kept because an earlier session mistook adding
    // this guard for a fix, when the early return had merely perturbed codegen; the
    // comment is here so nobody reads the guard as evidence of anything.
    uint pixelIndex;
    uint slot;
    UnpackPrepassWorkItem(ptPrepassWorkBuffer[index], pixelIndex, slot);
    if(pixelIndex >= viewport.x * viewport.y)
    {
      return;
    }
    ProcessPrepassPair(uint2(pixelIndex % viewport.x, pixelIndex / viewport.x), slot, viewport, sceneInfo);
    return;
  }

  // Unsorted: one invocation per pixel, looping every slot. Retained so the
  // sorting can be measured against the arrangement it replaces.
  const uint2 launchID = DispatchRaysIndex().xy;
  if(launchID.x >= viewport.x || launchID.y >= viewport.y)
  {
    return;
  }

  const uint slotCount = min(ptParams.spatialResampling.numSamples, uint(RESTIR_PT_MAX_PAIRING_TEXTURES));
  for(uint slot = 0; slot < RESTIR_PT_MAX_PAIRING_TEXTURES; ++slot)
  {
    // Every slot is written, including the ones past the requested count: the
    // resampling pass reads a partner's record without knowing whether that
    // partner ran, so a stale record would be read as a live shift.
    if(slot < slotCount)
    {
      ProcessPrepassPair(launchID, slot, viewport, sceneInfo);
    }
    else
    {
      ptPairedShiftBuffer[PTPairedShiftIndex(launchID, slot, viewport)] =
          MakeEmptyPTPairedShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoSource));
    }
  }
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
