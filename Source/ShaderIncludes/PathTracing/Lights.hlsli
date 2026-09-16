#ifndef PATH_TRACING_LIGHTS_H
#define PATH_TRACING_LIGHTS_H

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"
#include "ShaderIncludes/PathTracing/Disney.hlsli"
#include "ShaderIncludes/PathTracing/Intersection.hlsli"
// Holds EvaluateEmissiveTriangleDiscreteProbability so presampling passes get the distribution without the ray tracing code below.
#include "ShaderIncludes/PathTracing/LightDistribution.hlsli"

// Emissive triangle sampling
// Next-event estimation toward emissive glTF triangles: pick a triangle from the CPU-built CDF, pick a uniform point on it, and MIS weight the result against BSDF sampling.

// Emission cosine toward the receiver.
// Double-sided emissives emit on both hemispheres; one-sided emissives only emit along the positive normal hemisphere.
float EvaluateEmissiveTriangleCosine(GltfMetallicRoughness material, float3 geometricNormal, float3 directionToReceiver)
{
  const float cosine = dot(normalize(geometricNormal), directionToReceiver);

  return material.doubleSided != 0 ? abs(cosine) : max(cosine, 0.0);
}

// Emitted radiance at a point on an emissive triangle.
// Uses the same material resolution path as regular hits so sampled light and visible emissive surfaces stay consistent.
float3 EvaluateEmissiveTriangleRadiance(GltfSceneInfo sceneInfo, EmissiveTriangleLight light, float2 texCoord)
{
  const GltfMetallicRoughness material = LoadDeviceArrayElement<GltfMetallicRoughness>(sceneInfo.materials, light.materialIndex);
  const float4               baseColor = ResolveBaseColor(material, texCoord);

  // A point cut away by the alpha mask is not part of the surface and cannot emit.
  if(material.alphaMode == GltfAlphaMode::eMask && baseColor.w < material.alphaCutoff)
  {
    return (float3)0.0;
  }

  return ResolveEmission(material, texCoord);
}

// Converts the discrete-light choice and area sample into a solid-angle PDF seen from the receiver.
float EvaluateEmissiveTriangleSolidAnglePdf(GltfSceneInfo sceneInfo, uint lightIndex, EmissiveTriangleLight light, float3 receiverPosition, float3 sampledPosition)
{
  // Area PDF inputs
  // The triangle is chosen with its discrete probability and the point uniformly over its area. Zero-probability and degenerate triangles have no usable density.

  const float discreteProbability = EvaluateEmissiveTriangleDiscreteProbability(sceneInfo, lightIndex);

  if(discreteProbability <= 0.0 || light.area <= 1.0e-6)
  {
    return 0.0;
  }

  const float3 toLight         = sampledPosition - receiverPosition;
  const float  distanceSquared = dot(toLight, toLight);

  if(distanceSquared <= 1.0e-8)
  {
    return 0.0;
  }

  const GltfMetallicRoughness material = LoadDeviceArrayElement<GltfMetallicRoughness>(sceneInfo.materials, light.materialIndex);
  const float3 lightDirection          = toLight * rsqrt(distanceSquared);
  const float  lightCos                = EvaluateEmissiveTriangleCosine(material, light.geometricNormal, -lightDirection);

  // A receiver behind a one-sided emitter cannot be reached by this light.
  if(lightCos <= 0.0)
  {
    return 0.0;
  }

  // Area to solid angle
  // p_omega = p_area * distance^2 / cos(theta_light).

  const float areaPdf = discreteProbability / light.area;

  return areaPdf * distanceSquared / max(lightCos, 1.0e-6);
}

// Light-sampling PDF for an emissive triangle that a BSDF path landed on, so the hit can be MIS-weighted against direct-light sampling.
// The triangle's light index is found by a linear scan of the emissive list.
float EvaluateCurrentEmissiveHitPdf(GltfSceneInfo sceneInfo, uint instanceIndex, uint primitiveIndex, float3 receiverPosition, float3 hitPosition)
{
  for(uint i = 0; i < sceneInfo.emissiveTriangleCount; ++i)
  {
    const EmissiveTriangleLight light = LoadDeviceArrayElement<EmissiveTriangleLight>(sceneInfo.emissiveTriangles, i);

    if(light.instanceIndex == instanceIndex && light.primitiveIndex == primitiveIndex)
    {
      return EvaluateEmissiveTriangleSolidAnglePdf(sceneInfo, i, light, receiverPosition, hitPosition);
    }
  }

  return 0.0;
}

// Whether emissive next event estimation takes a light sample at this surface. Like environment NEE, it is only applied on opaque-style hits in this tracer.
// Every emissive NEE site and every emissive-hit MIS weight must use this same test: weighting a BSDF-found emitter against a light sample that was never taken loses the energy the weight removes.
bool CanSampleEmissiveDirectLight(SurfaceData surface, GltfSceneInfo sceneInfo)
{
  return surface.transmission <= 0.001 && sceneInfo.emissiveTriangleCount > 0;
}

// One emissive next-event-estimation sample at a path vertex. instanceIndex and primitiveIndex name the vertex's own triangle.
void AccumulateEmissiveDirectLight(inout PathState path, SurfaceData surface, uint instanceIndex, uint primitiveIndex, GltfSceneInfo sceneInfo, float3 viewDir)
{
  // Light selection

  if(!CanSampleEmissiveDirectLight(surface, sceneInfo))
  {
    return;
  }

  const uint sampleIndex            = BinarySearchCdf(sceneInfo.emissiveTriangleCdf, sceneInfo.emissiveTriangleCount, NextRandom(path.seed));
  const EmissiveTriangleLight light = LoadDeviceArrayElement<EmissiveTriangleLight>(sceneInfo.emissiveTriangles, sampleIndex);

  // Prevent a surface from explicitly re-sampling itself as a direct light.
  if(light.instanceIndex == instanceIndex && light.primitiveIndex == primitiveIndex)
  {
    return;
  }

  // Point on the light
  // Uniform barycentric triangle sampling inside the chosen emissive primitive: the square root warps the unit square so area density is constant.

  const float sqrtXi0 = sqrt(NextRandom(path.seed));
  const float xi1     = NextRandom(path.seed);
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

  // Shadow geometry
  // All direct-light geometry, PDFs, and visibility tests must agree on the same offset origin to avoid self-occlusion.

  const float3 shadowOrigin    = OffsetRay(surface.worldPosition, SelectOffsetNormal(surface.geometricNormal, unoffsetToLight));
  const float3 toLight         = sampledPosition - shadowOrigin;
  const float  distanceSquared = dot(toLight, toLight);

  if(distanceSquared <= 1.0e-8)
  {
    return;
  }

  const float  distance                 = sqrt(distanceSquared);
  const float3 lightDir                 = toLight / distance;
  const GltfMetallicRoughness material  = LoadDeviceArrayElement<GltfMetallicRoughness>(sceneInfo.materials, light.materialIndex);
  const float  lightCos                 = EvaluateEmissiveTriangleCosine(material, light.geometricNormal, -lightDir);

  if(lightCos <= 0.0)
  {
    return;
  }

  const float lightPdf = EvaluateEmissiveTriangleSolidAnglePdf(sceneInfo, sampleIndex, light, shadowOrigin, sampledPosition);

  if(lightPdf <= 0.0)
  {
    return;
  }

  // The visibility ray stops just short of the light so the emitter itself does not count as an occluder.
  const float maxDistance = max(distance - 0.001, 0.001);

  if(!TraceVisibilityFromOrigin(shadowOrigin, lightDir, maxDistance))
  {
    return;
  }

  // BSDF and MIS
  // Explicit emissive sampling estimates only the broad cosine-proposal BSDF group, so MIS only competes against broad BSDF samples.
  // The returned bsdf already includes the receiver cosine. MIS keeps the direct-light sample and BSDF-hit path from double counting the same emissive contribution.

  float  bsdfPdf = 0.0;
  float3 bsdf    = EvaluateDirectLightBsdf(surface, viewDir, lightDir, bsdfPdf);

  if(bsdfPdf <= 0.0 || SafeMax3(bsdf) <= 0.0)
  {
    return;
  }

  const float misWeight     = MisMixWeight(lightPdf, bsdfPdf);
  const float3 contribution = path.throughput * radiance * bsdf * (misWeight / lightPdf);

  // Publishes the light vertex before the contribution is accumulated, so a resampler can adopt it as a reconnection anchor.
  // Short paths, above all direct lighting at the primary hit, have no interior vertex to reconnect at, and without this they cannot be shifted at all.
  PATH_TRACING_ON_NEE_EMISSIVE(path, sampleIndex, bary, radiance, lightPdf, distance);

  AccumulatePathContribution(path, contribution);
  StoreDiffuseDenoiserHitDistanceIfMissing(path, distance);
}

#endif

