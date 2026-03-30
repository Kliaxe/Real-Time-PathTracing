#ifndef PATH_TRACING_DIRECT_LIGHTING_H
#define PATH_TRACING_DIRECT_LIGHTING_H

#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"

float3 ComputeSurfaceBaseRadiance(SurfaceData surface, float3 viewDir, inout uint seed)
{
  PathPayload lightingPayload;
  lightingPayload                          = (PathPayload)0;
  lightingPayload.radiance                 = surface.emission;
  lightingPayload.throughput               = float3(1.0);
  lightingPayload.mediumAttenuation        = float3(0.0);
  lightingPayload.mediumThickness          = 0.0;
  lightingPayload.lastSurfacePosition      = surface.worldPosition;
  lightingPayload.lastSurfaceNormal        = surface.shadingNormal;
  lightingPayload.lastDirectLightBsdfPdf   = 0.0;
  lightingPayload.seed                     = seed;
  lightingPayload.depth                    = 0;
  lightingPayload.mediumActive             = 0;
  lightingPayload.lastEnvironmentNeeActive = 0;
  lightingPayload.hasLastBsdfSample        = 0;

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  AccumulateEnvironmentDirectLight(lightingPayload, surface, sceneInfo, viewDir);
  AccumulateEmissiveDirectLight(lightingPayload, surface, sceneInfo, viewDir);
  seed = lightingPayload.seed;

  return lightingPayload.radiance;
}

#endif
