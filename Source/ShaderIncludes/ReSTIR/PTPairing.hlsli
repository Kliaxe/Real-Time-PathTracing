#ifndef RESTIR_PT_PAIRING_H
#define RESTIR_PT_PAIRING_H

// Pairing helpers
// Pairing and paired-shift-record helpers with no ray tracing dependency.
// Split out of PTSpatialCommon.hlsli so the Section 6.2.2 compute passes can use them: that header builds on the shift machinery, whose TraceRay is not valid outside a ray tracing stage, and those passes need the pairing decisions without the tracing.
// Everything here must stay pure for that reason. A helper that needs to trace belongs next door, not in this file.

#include <Common/ShaderTypes.h>
#include "ShaderIo.h"
#include "ReSTIR/PTParameters.h"
// IsValidNeighbor and IsMaterialSimilar.
#include "ShaderIncludes/ReSTIR/Common.hlsli"

// Packs the validity flag alongside the outcome code, so a record can say "the pair formed but the shift failed" distinctly from "no pair".
static const uint kPTPairedShiftValidBit = 0x80000000u;

// Positive modulus. Pairing textures tile, so every coordinate is toroidal and a negative intermediate must wrap rather than clamp.
int PTWrapCoordinate(int value, int size)
{
  const int wrapped = value % size;

  return wrapped < 0 ? wrapped + size : wrapped;
}

// Finds the pixel paired with `pixel` in slot `slot`, if any.
// The pairing texture is an involution on its own torus. Applying it directly every frame would pair the same two pixels forever, so the stored involution is conjugated by a per-frame symmetry g: partner(p) = g(T(g_inverse(p))).
// A conjugated involution is still an involution, g(T(g_inv(g(T(g_inv(p)))))) = p, so reciprocity survives, which is what the shared-shift trick depends on.
bool PTFindPairedNeighbour(uint2 pixel, uint slot, uint2 viewport, out int2 partnerPixel)
{
  partnerPixel = (int2)pixel;

  const ReSTIRPTPairingTextureParameters pairing = ptParams.pairingTextures[slot];

  // A zero size marks a slot whose pairing texture is unusable.
  if(pairing.size == 0)
  {
    return false;
  }

  const int size = int(pairing.size);

  // Screen space to texture space
  // Translate, then optionally swap axes, then optionally flip.

  const int translationX = int(pairing.translation & 0xffffu);
  const int translationY = int((pairing.translation >> 16) & 0xffffu);

  int2 texel = int2(PTWrapCoordinate(int(pixel.x) + translationX, size), PTWrapCoordinate(int(pixel.y) + translationY, size));

  const bool swapAxes = (pairing.transform & 0x1u) != 0u;
  const bool flipX    = (pairing.transform & 0x2u) != 0u;
  const bool flipY    = (pairing.transform & 0x4u) != 0u;

  if(swapAxes)
  {
    texel = texel.yx;
  }

  // Reflection on a discrete torus is (size - v) mod size, which fixes 0 and is its own inverse: both required for the conjugation to stay a bijection.
  if(flipX)
  {
    texel.x = PTWrapCoordinate(size - texel.x, size);
  }

  if(flipY)
  {
    texel.y = PTWrapCoordinate(size - texel.y, size);
  }

  const uint packed = ptPairingBuffer[pairing.bufferOffset + uint(texel.y) * uint(size) + uint(texel.x)];

  // Deltas are stored biased by 128 so they survive an unsigned round trip.
  int2 delta = int2(int(packed & 0xffu) - 128, int((packed >> 8) & 0xffu) - 128);

  // Texture space to screen space
  // The delta is mapped back by undoing the linear part in reverse order: flips were applied last going in, so they are undone first coming out.
  // Translation has no linear part and so does not touch the delta.

  if(flipX)
  {
    delta.x = -delta.x;
  }

  if(flipY)
  {
    delta.y = -delta.y;
  }

  if(swapAxes)
  {
    delta = delta.yx;
  }

  // Partner validation

  if(all(delta == int2(0, 0)))
  {
    return false;
  }

  const int2 candidate = (int2)pixel + delta;

  // A partner outside the viewport simply has no data. Wrapping to the far edge instead would pair pixels across unrelated geometry.
  if(any(candidate < int2(0, 0)) || any(candidate >= (int2)viewport))
  {
    return false;
  }

  partnerPixel = candidate;

  return true;
}

// Symmetric compatibility test for a spatial pair.
// Every term must be symmetric in the two pixels, or A could accept B while B rejects A.
// That asymmetry would not break unbiasedness, since each pixel's MIS weights are computed over its own accepted set, but it would waste the shared shift: one partner would compute a record nobody reads.
bool PTArePixelsCompatible(ReSTIRPTSurface a, ReSTIRPTSurface b)
{
  if(a.valid == 0 || b.valid == 0)
  {
    return false;
  }

  if(!IsValidNeighbor(a.shadingNormal, b.shadingNormal, a.linearDepth, b.linearDepth, ptParams.spatialResampling.normalThreshold, ptParams.spatialResampling.depthThreshold))
  {
    return false;
  }

  if(ptParams.spatialResampling.enableMaterialSimilarityTest != 0u && !IsMaterialSimilar(a.roughness, b.roughness, a.metallic, b.metallic, a.albedo, b.albedo))
  {
    return false;
  }

  return true;
}

ReSTIRPTPairedShift MakeEmptyPTPairedShift(uint outcome)
{
  ReSTIRPTPairedShift record;

  record.integrand              = (float3)0.0;
  record.jacobian               = 0.0;
  record.destinationDenominator = 0.0;
  record.outcome                = outcome;

  return record;
}

bool IsPTPairedShiftValid(ReSTIRPTPairedShift record)
{
  return (record.outcome & kPTPairedShiftValidBit) != 0u;
}

uint PTPairedShiftOutcome(ReSTIRPTPairedShift record)
{
  return record.outcome & ~kPTPairedShiftValidBit;
}

// One record per pixel per slot, slot-major within a pixel so a pixel's slots are contiguous and its partner's record for the same slot is a single strided read.
uint PTPairedShiftIndex(uint2 pixel, uint slot, uint2 viewport)
{
  return (uint(pixel.y) * viewport.x + uint(pixel.x)) * RESTIR_PT_MAX_PAIRING_TEXTURES + slot;
}

#endif
