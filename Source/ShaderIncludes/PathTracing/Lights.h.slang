#ifndef PATH_TRACING_LIGHTS_H
#define PATH_TRACING_LIGHTS_H

#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"

// -------------------------------------------------------------------------
//    Emissive triangle sampling
// -------------------------------------------------------------------------

float EvaluateEmissiveTriangleDiscreteProbability(GltfSceneInfo sceneInfo, uint lightIndex)
{
  // Reconstructs one light's discrete probability mass from the normalized prefix CDF built on the CPU.
  if(sceneInfo.emissiveTriangleCount == 0 || lightIndex >= sceneInfo.emissiveTriangleCount)
  {
    return 0.0;
  }

  const float previous = lightIndex > 0 ? sceneInfo.emissiveTriangleCdf[lightIndex - 1] : 0.0;
  return max(sceneInfo.emissiveTriangleCdf[lightIndex] - previous, 0.0);
}

float EvaluateEmissiveTriangleCosine(GltfMetallicRoughness material, float3 geometricNormal, float3 directionToReceiver)
{
  // Double-sided emissives emit on both hemispheres; one-sided emissives only emit along the positive normal hemisphere.
  const float cosine = dot(normalize(geometricNormal), directionToReceiver);
  return material.doubleSided != 0 ? abs(cosine) : max(cosine, 0.0);
}

float3 EvaluateEmissiveTriangleRadiance(GltfSceneInfo sceneInfo, PathTraceEmissiveTriangle light, float2 texCoord)
{
  // Uses the same material resolution path as regular hits so sampled light and visible emissive surfaces stay consistent.
  const GltfMetallicRoughness material = sceneInfo.materials[light.materialIndex];
  const float4               baseColor = ResolveBaseColor(material, texCoord);
  if(material.alphaMode == GltfAlphaMode::eMask && baseColor.w < material.alphaCutoff)
  {
    return float3(0.0);
  }

  return ResolveEmission(material, texCoord);
}

float EvaluateEmissiveTriangleSolidAnglePdf(GltfSceneInfo sceneInfo,
                                            uint lightIndex,
                                            PathTraceEmissiveTriangle light,
                                            float3 receiverPosition,
                                            float3 sampledPosition)
{
  // Converts the discrete-light choice and area sample into a solid-angle PDF seen from the receiver.
  const float discreteProbability = EvaluateEmissiveTriangleDiscreteProbability(sceneInfo, lightIndex);
  if(discreteProbability <= 0.0 || light.area <= 1.0e-6)
  {
    return 0.0;
  }

  const float3 toLight          = sampledPosition - receiverPosition;
  const float  distanceSquared  = dot(toLight, toLight);
  if(distanceSquared <= 1.0e-8)
  {
    return 0.0;
  }

  const GltfMetallicRoughness material = sceneInfo.materials[light.materialIndex];
  const float3 lightDirection          = toLight * rsqrt(distanceSquared);
  const float  lightCos                = EvaluateEmissiveTriangleCosine(material, light.geometricNormal, -lightDirection);
  if(lightCos <= 0.0)
  {
    return 0.0;
  }

  const float areaPdf = discreteProbability / light.area;
  return areaPdf * distanceSquared / max(lightCos, 1.0e-6);
}

float EvaluateCurrentEmissiveHitPdf(GltfSceneInfo sceneInfo, uint instanceIndex, uint primitiveIndex, float3 receiverPosition, float3 hitPosition)
{
  // Used when a BSDF path lands on an emissive triangle so the hit can be MIS-weighted against direct-light sampling.
  for(uint i = 0; i < sceneInfo.emissiveTriangleCount; ++i)
  {
    const PathTraceEmissiveTriangle light = sceneInfo.emissiveTriangles[i];
    if(light.instanceIndex == instanceIndex && light.primitiveIndex == primitiveIndex)
    {
      return EvaluateEmissiveTriangleSolidAnglePdf(sceneInfo, i, light, receiverPosition, hitPosition);
    }
  }

  return 0.0;
}

void AccumulateEmissiveDirectLight(inout PathPayload payload, SurfaceData surface, GltfSceneInfo sceneInfo, float3 viewDir)
{
  // Like environment NEE, explicit emissive sampling is only applied on opaque-style hits in this tracer.
  if(surface.transmission > 0.001 || sceneInfo.emissiveTriangleCount == 0)
  {
    return;
  }

  const uint sampleIndex = BinarySearchCdf(sceneInfo.emissiveTriangleCdf, sceneInfo.emissiveTriangleCount, NextRandom(payload.seed));
  const PathTraceEmissiveTriangle light = sceneInfo.emissiveTriangles[sampleIndex];
  if(light.instanceIndex == InstanceIndex() && light.primitiveIndex == PrimitiveIndex())
  {
    // Prevent a surface from explicitly re-sampling itself as a direct light.
    return;
  }

  // Uniform barycentric triangle sampling inside the chosen emissive primitive.
  const float sqrtXi0 = sqrt(NextRandom(payload.seed));
  const float xi1     = NextRandom(payload.seed);
  const float3 bary   = float3(1.0 - sqrtXi0, sqrtXi0 * (1.0 - xi1), sqrtXi0 * xi1);

  const float3 sampledPosition = light.position0 * bary.x + light.position1 * bary.y + light.position2 * bary.z;
  const float2 sampledTexCoord = light.texCoord0 * bary.x + light.texCoord1 * bary.y + light.texCoord2 * bary.z;
  const float3 radiance        = EvaluateEmissiveTriangleRadiance(sceneInfo, light, sampledTexCoord);
  if(SafeMax3(radiance) <= 0.0)
  {
    return;
  }

  const float3 unoffsetToLight = sampledPosition - surface.worldPosition;
  if(dot(unoffsetToLight, unoffsetToLight) <= 1.0e-8)
  {
    return;
  }

  // All direct-light geometry, PDFs, and visibility tests must agree on the same offset origin to avoid self-occlusion.
  const float3 shadowOrigin    = OffsetRay(surface.worldPosition, SelectOffsetNormal(surface.geometricNormal, unoffsetToLight));
  const float3 toLight         = sampledPosition - shadowOrigin;
  const float  distanceSquared = dot(toLight, toLight);
  if(distanceSquared <= 1.0e-8)
  {
    return;
  }

  const float  distance    = sqrt(distanceSquared);
  const float3 lightDir    = toLight / distance;
  const GltfMetallicRoughness material = sceneInfo.materials[light.materialIndex];
  const float  lightCos                = EvaluateEmissiveTriangleCosine(material, light.geometricNormal, -lightDir);
  if(lightCos <= 0.0)
  {
    return;
  }

  const float lightPdf = EvaluateEmissiveTriangleSolidAnglePdf(sceneInfo, sampleIndex, light, shadowOrigin, sampledPosition);
  if(lightPdf <= 0.0)
  {
    return;
  }

  const float maxDistance = max(distance - 0.001, 0.001);
  if(!TraceVisibilityFromOrigin(shadowOrigin, lightDir, maxDistance))
  {
    return;
  }

  // EvaluateSurfaceBsdf already returns f * |N dot L|, so the estimator divides only by the light PDF here.
  float  bsdfPdf = 0.0;
  float3 bsdf    = EvaluateSurfaceBsdf(surface, viewDir, lightDir, bsdfPdf);
  if(bsdfPdf <= 0.0 || SafeMax3(bsdf) <= 0.0)
  {
    return;
  }

  // MIS keeps the direct-light sample and BSDF-hit path from double counting the same emissive contribution.
  const float misWeight = MisMixWeight(lightPdf, bsdfPdf);
  payload.radiance += payload.throughput * radiance * bsdf * (misWeight / lightPdf);
}

#endif
