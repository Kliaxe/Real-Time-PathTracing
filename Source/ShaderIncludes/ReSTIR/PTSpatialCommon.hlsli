#ifndef RESTIR_PT_SPATIAL_COMMON_H
#define RESTIR_PT_SPATIAL_COMMON_H

#include "ReSTIR/PTPairing.hlsli"

// Spatial reuse helpers
// Shared by the spatial pre-pass and the spatial resampling pass.
// These live together because Section 3 splits spatial reuse into two passes that must agree exactly on who is paired with whom and on what surface a shift lands.
// Any divergence between the two would silently break reciprocity: the pre-pass would compute a shift into one domain while the resampling pass read it as a shift into another.

// Uniform sample inside a disk of the configured radius, in pixels.
// Uniform rather than Gaussian on purpose: this is the unpaired reference that Section 3's pairing textures are measured against, and they are matched on mean sample distance to a uniform disk of the same radius.
int2 SampleNeighbourOffset(inout uint seed, float radius)
{
  const float angle          = NextRandom(seed) * 2.0 * 3.14159265358979;
  const float radiusFraction = sqrt(NextRandom(seed));

  return int2(round(float2(cos(angle), sin(angle)) * (radiusFraction * radius)));
}

// Recovers a full material record at a stored surface point.
// The stored ReSTIRPTSurface is deliberately reduced and cannot drive a BSDF, so the point is re-traced from the camera.
// The result is a genuine closest-hit surface, identical in fidelity to a freshly traced primary hit, which is what makes it safe for a shift to land on.
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

  const float3        direction = toSurface / distance;
  const PTVertexQuery query     = TracePTVertex(cameraPosition, direction);

  if(!query.hit)
  {
    return false;
  }

  // The stored point must still be the first thing the ray meets, or this is a different surface that merely lies along the same direction.
  if(length(query.surface.worldPosition - stored.worldPosition) > 0.01 * distance)
  {
    return false;
  }

  surface = query.surface;
  viewDir = -direction;

  return true;
}

ReSTIRPTPairedShift MakePTPairedShift(PTShiftResult result)
{
  ReSTIRPTPairedShift record;

  record.integrand              = result.integrand;
  record.jacobian               = result.jacobian;
  record.destinationDenominator = result.destinationDenominator;
  record.outcome                = result.outcome | (result.valid ? kPTPairedShiftValidBit : 0u);

  return record;
}

// Re-inflates a stored record into the shift result the resampling math expects, so the MIS code is identical whether a shift was just computed or read back.
// sourceDenominator is not stored: it exists only to decompose an extreme Jacobian in diagnostics, and the ratio it belongs to is already resolved here.
PTShiftResult PTPairedShiftToResult(ReSTIRPTPairedShift record)
{
  PTShiftResult result;

  result.integrand              = record.integrand;
  result.jacobian               = record.jacobian;
  result.destinationDenominator = record.destinationDenominator;
  result.sourceDenominator      = 0.0;
  result.valid                  = IsPTPairedShiftValid(record);
  result.outcome                = PTPairedShiftOutcome(record);

  return result;
}

#endif


