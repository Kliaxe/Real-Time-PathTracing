#ifndef RESTIR_RAY_QUERY_VISIBILITY_H
#define RESTIR_RAY_QUERY_VISIBILITY_H

#include "ShaderIncludes/ReSTIR/PT/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"

bool TraceReSTIRPTVisibility(float3 worldPosition, float3 geometricNormal, float3 direction, float maxDistance)
{
  // Shift validation only needs a binary occlusion result, so an inline ray
  // query keeps the compute passes self-contained.
  if(maxDistance <= 1.0e-4)
  {
    return true;
  }

  RayDesc ray;
  ray.Origin    = OffsetRay(worldPosition, SelectOffsetNormal(geometricNormal, direction));
  ray.Direction = direction;
  ray.TMin      = 0.0;
  ray.TMax      = maxDistance;

  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(topLevelAS, RAY_FLAG_NONE, 0xFF, ray);

  while(query.Proceed())
  {
    if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
    {
      // ReSTIR PT currently treats masked geometry as solid during shift
      // validation. That is conservative for reuse and keeps the compute pass
      // independent of the full any-hit material path.
      query.CommitNonOpaqueTriangleHit();
    }
  }

  return query.CommittedStatus() == COMMITTED_NOTHING;
}

#endif
