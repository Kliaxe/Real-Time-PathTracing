// Spatial reuse pre-pass
// ReSTIR PT Enhanced, Section 3. Writes, per pixel and per paired neighbour slot, the shift of this pixel's path into its partner's domain.
// Contract with the resampling pass: pairing is reciprocal, so a pair's two shifts are the same two computations both partners need. Each pixel computes one; the resampling pass then reads its own record as the inverse shift and its partner's as the forward shift, tracing neither. Two shifts per pair instead of four.
// A pixel cannot read a record the same dispatch is still writing, which is what forces the split into two passes and the barrier between them.

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
#include "ReSTIR/PTPrepassSort.hlsli"

// One (pixel, slot) pair: rebuild the partner's surface and shift this pixel's path into it. Shared by both dispatch shapes below so they cannot drift apart.
// The partner surface is rebuilt from the hit identity its pixel stored, so this costs the scene reads of one surface and no ray at all; it used to trace back at the stored point, which is what the classify pass was once measured against hoisting.
void ProcessPrepassPair(uint2 launchID, uint slot, uint2 viewport, GltfSceneInfo sceneInfo)
{
  const uint              pixelIndex           = launchID.y * viewport.x + launchID.x;
  const ReSTIRPTSurface   currentSurfaceRecord = currentSurfaceBuffer[pixelIndex];
  const ReSTIRPTReservoir canonical            = LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(launchID), ptParams.bufferIndices.spatialResamplingInputBufferIndex);

  // Every exit below writes a record, so a pair that never forms still reads back as "no source".
  ReSTIRPTPairedShift record = MakeEmptyPTPairedShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoSource));

  int2 partnerPixel;

  if(currentSurfaceRecord.valid != 0 && IsValidPTReservoir(canonical) && PTFindPairedNeighbour(launchID, slot, viewport, partnerPixel))
  {
    const uint            partnerIndex   = uint(partnerPixel.y) * viewport.x + uint(partnerPixel.x);
    const ReSTIRPTSurface partnerSurface = currentSurfaceBuffer[partnerIndex];

    if(PTArePixelsCompatible(currentSurfaceRecord, partnerSurface))
    {
      SurfaceData partnerPrimarySurface;
      float3      partnerPrimaryViewDir;

      // The compatibility test above already required a valid partner record, so the rebuild cannot fail and its result is not tested.
      LoadPTSurfaceAtStoredHit(partnerSurface, sceneInfo.cameraPosition, partnerPrimarySurface, partnerPrimaryViewDir);

      record = MakePTPairedShift(ShiftPathToSurface(canonical, partnerPrimarySurface, partnerPrimaryViewDir));
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
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();
  const uint2         viewport  = (uint2)sceneInfo.viewportSize;

  // Compacted dispatch
  // Section 6.2.2. One invocation per surviving pair, launched indirectly over the work list the classify pass built. eReSTIRPTFlagSortedPrepass keeps its historical name, but the list is compacted, not sorted by divergence class (see ReSTIRPTPrepassClassify.hlsl).
  // The branch is on a push constant, so it is uniform across the dispatch and costs nothing.

  if((pushConst.flags & uint(ReSTIRPTFlags::eReSTIRPTFlagSortedPrepass)) != 0u)
  {
    const uint index = DispatchRaysIndex().x;

    // Strictly redundant: a ray dispatch launches exactly the requested dimensions, with no rounded-up workgroup tail to guard against. Kept as a cheap assertion that the launch size and the list agree, since a mismatch between them would otherwise read unwritten entries silently.
    if(index >= ptPrepassCounterBuffer[RESTIR_PT_PREPASS_INDIRECT_OFFSET])
    {
      return;
    }

    uint pixelIndex;
    uint slot;

    UnpackPrepassWorkItem(ptPrepassWorkBuffer[index], pixelIndex, slot);

    // Bounds check on the unpacked pixel index. Measured as never triggering (an instrumented run found 0 of 797223 entries out of range), so this is an assertion, not a fix. It is kept because an earlier session mistook adding this guard for a fix, when the early return had merely perturbed codegen; the comment is here so nobody reads the guard as evidence of anything.
    if(pixelIndex >= viewport.x * viewport.y)
    {
      return;
    }

    ProcessPrepassPair(uint2(pixelIndex % viewport.x, pixelIndex / viewport.x), slot, viewport, sceneInfo);

    return;
  }

  // Per-pixel dispatch
  // One invocation per pixel, looping every slot. Retained so the compacted dispatch can be measured against the arrangement it replaces.

  const uint2 launchID = DispatchRaysIndex().xy;

  if(launchID.x >= viewport.x || launchID.y >= viewport.y)
  {
    return;
  }

  const uint slotCount = min(ptParams.spatialResampling.numSamples, uint(RESTIR_PT_MAX_PAIRING_TEXTURES));

  for(uint slot = 0; slot < RESTIR_PT_MAX_PAIRING_TEXTURES; ++slot)
  {
    // Every slot is written, including the ones past the requested count: the resampling pass reads a partner's record without knowing whether that partner ran, so a stale record would be read as a live shift.
    if(slot < slotCount)
    {
      ProcessPrepassPair(launchID, slot, viewport, sceneInfo);
    }
    else
    {
      ptPairedShiftBuffer[PTPairedShiftIndex(launchID, slot, viewport)] = MakeEmptyPTPairedShift(uint(ReSTIRPTShiftOutcome::eReSTIRPTShiftOutcomeNoSource));
    }
  }
}

