#ifndef PATH_TRACING_PATH_RAY_ENTRY_POINTS_H
#define PATH_TRACING_PATH_RAY_ENTRY_POINTS_H

// Path ray entry points
// The hit and miss shaders of every pipeline that traces path rays, written once.
// Path rays only record what they hit, so ray generation can resolve the surface itself with LoadSurfaceDataFromHit; shadow rays skip closest hit and learn visibility from their miss shader. Both any-hit shaders cut alpha-masked holes.
// They live apart from PathLoop.hlsli because the reuse passes trace the same rays without running the loop: what the two share is the payload contract, not the estimator.

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Intersection.hlsli"

[shader("miss")]
void rmissMain(inout PathHitRecord hit)
{
  hit.hasHit = 0u;
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload)
{
  payload.visible = 1;
}

[shader("anyhit")]
void rahitMain(inout PathHitRecord hit, in BuiltInTriangleIntersectionAttributes attr)
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
void rchitMain(inout PathHitRecord hit, in BuiltInTriangleIntersectionAttributes attr)
{
  hit = MakePathHitRecord(attr);
}

#endif
