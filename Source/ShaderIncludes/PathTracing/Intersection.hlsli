#ifndef PATH_TRACING_INTERSECTION_H
#define PATH_TRACING_INTERSECTION_H

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"
#include "ShaderIncludes/PathTracing/Hdri.hlsli"
#include "ShaderIncludes/PathTracing/Disney.hlsli"
#include "ShaderIncludes/SceneAccess.hlsli"

// Geometry access
// Material texture resolution and vertex attribute fetches that turn raw glTF data at a hit into shading inputs.

// Resolves glTF metallic and roughness for a texture coordinate.
// glTF packs roughness in G and metallic in B, with scalar factors acting as post-multipliers.
float2 ResolveMetallicRoughness(GltfMetallicRoughness material, float2 texCoord)
{
  float metallic  = material.metallicFactor;
  float roughness = material.roughnessFactor;

  if(material.metallicRoughnessTextureIndex > -1)
  {
    const float4 metallicRoughnessSample = SampleSceneTextureLevel(material.metallicRoughnessTextureIndex, texCoord, 0.0);

    roughness *= metallicRoughnessSample.g;
    metallic  *= metallicRoughnessSample.b;
  }

  return clamp(float2(metallic, roughness), 0.0, 1.0);
}

// Small helper for extension textures that store one scalar in one known channel.
// Note that a present texture replaces the fallback instead of multiplying it.
float ResolveTextureChannel(int textureIndex, float2 texCoord, int channel, float fallback)
{
  if(textureIndex < 0)
  {
    return fallback;
  }

  const float4 sample = SampleSceneTextureLevel(textureIndex, texCoord, 0.0);

  if(channel == 0)
    return sample.x;
  if(channel == 1)
    return sample.y;
  if(channel == 2)
    return sample.z;
  if(channel == 3)
    return sample.w;
  return fallback;
}

// Small helper for extension textures that modulate an RGB factor directly.
float3 ResolveTextureColor(int textureIndex, float2 texCoord, float3 fallback)
{
  if(textureIndex < 0)
  {
    return fallback;
  }

  return fallback * SampleSceneTextureLevel(textureIndex, texCoord, 0.0).xyz;
}

// Interprets a tint texture as a color magnitude control and collapses it back to a scalar tint amount.
float ResolveTintMagnitude(int textureIndex, float2 texCoord, float fallback)
{
  if(textureIndex < 0)
  {
    return fallback;
  }

  const float3 sample = SampleSceneTextureLevel(textureIndex, texCoord, 0.0).xyz;

  return fallback * length(sample);
}

// glTF emissive texture modulates the emissive factor instead of replacing it.
float3 ResolveEmission(GltfMetallicRoughness material, float2 texCoord)
{
  float3 emission = max(material.emissionFactor, (float3)0.0);

  if(material.emissiveTextureIndex >= 0)
  {
    emission *= SampleSceneTextureLevel(material.emissiveTextureIndex, texCoord, 0.0).xyz;
  }

  return max(emission, (float3)0.0);
}

// Base color stays in RGBA because alpha masking still depends on the A channel.
float4 ResolveBaseColor(GltfMetallicRoughness material, float2 texCoord)
{
  float4 baseColor = material.baseColorFactor;

  if(material.baseColorTextureIndex >= 0)
  {
    baseColor *= SampleSceneTextureLevel(material.baseColorTextureIndex, texCoord, 0.0);
  }

  return baseColor;
}

// The raw glTF buffer is already on the GPU, so hit shaders fetch attributes directly through the uploaded byte layout.
template<typename T> T getAttribute(uint64_t dataBufferAddress, BufferView bufferView, uint attributeIndex)
{
  return LoadAttribute<T>(dataBufferAddress, bufferView, attributeIndex);
}

// glTF indices are unsigned; keep them unsigned so large 16-bit meshes do not sign-extend.
uint3 getTriangleIndices(uint64_t dataBufferAddress, const TriangleMesh mesh, uint primitiveID)
{
  return LoadTriangleIndices(dataBufferAddress, mesh, primitiveID);
}

// Hit attributes are reconstructed by barycentric interpolation in object-space vertex order.
template<typename T> T getTriangleAttribute(uint64_t dataBufferAddress, BufferView bufferView, uint3 attributeIndex, float3 barycentrics)
{
  return LoadTriangleAttribute<T>(dataBufferAddress, bufferView, attributeIndex, barycentrics);
}

// Builds the world-space tangent frame at a hit around an already face-forwarded shading normal.
void BuildSurfaceBasis(GltfMesh mesh, uint3 indices, float3 barycentrics, float3 shadingNormal, out float3 tangent, out float3 bitangent)
{
  // If the asset has no tangent frame, build one procedurally so anisotropy / normal mapping still have a stable basis.
  if(mesh.triMesh.tangents.count == 0)
  {
    BuildOrthonormalBasis(shadingNormal, tangent, bitangent);
    return;
  }

  // Authored tangent
  // The tangent is transformed as a direction and Gram-Schmidt projected off the shading normal. Its w component carries the glTF bitangent handedness.

  const float4 tangentSample = getTriangleAttribute<float4>(mesh.gltfBuffer, mesh.triMesh.tangents, indices, barycentrics);

  tangent                     = normalize(mul(float4(tangentSample.xyz, 0.0), ObjectToWorld4x3()));
  tangent                     = tangent - shadingNormal * dot(shadingNormal, tangent);

  // Degenerate authored tangents are treated like missing tangents.
  if(dot(tangent, tangent) <= 1.0e-6)
  {
    BuildOrthonormalBasis(shadingNormal, tangent, bitangent);
    return;
  }

  tangent   = normalize(tangent);
  bitangent = normalize(cross(shadingNormal, tangent) * tangentSample.w);
}

// Tangent-space normal maps are decoded from [0, 1] into [-1, 1] before world-space reconstruction.
float3 SampleNormalMap(int textureIndex, float2 texCoord, float3 shadingNormal, float3 tangent, float3 bitangent)
{
  const float3 normalSample = SampleSceneTextureLevel(textureIndex, texCoord, 0.0).xyz * 2.0 - 1.0;
  const float3 mappedNormal = tangent * normalSample.x + bitangent * normalSample.y + shadingNormal * normalSample.z;

  return normalize(mappedNormal);
}

// Alpha test for the any-hit shaders.
// They use this to punch holes in cutout materials without treating blend materials as stochastic transparency.
bool IsMaskedSurfaceHit(BuiltInTriangleIntersectionAttributes attr)
{
  const float3 barycentrics   = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
  const uint   instanceIndex  = InstanceIndex();
  const uint   primitiveIndex = PrimitiveIndex();

  const GltfSceneInfo         sceneInfo = pushConst.sceneInfoAddress.Get();
  const GltfInstance          instance  = LoadDeviceArrayElement<GltfInstance>(sceneInfo.instances, instanceIndex);
  const GltfMesh              mesh      = LoadDeviceArrayElement<GltfMesh>(sceneInfo.meshes, instance.meshIndex);
  const GltfMetallicRoughness material  = LoadDeviceArrayElement<GltfMetallicRoughness>(sceneInfo.materials, instance.materialIndex);

  if(material.alphaMode != GltfAlphaMode::eMask)
  {
    return false;
  }

  // Without texture coordinates only the constant base color alpha can cut the surface.
  if(mesh.triMesh.texCoords.count == 0)
  {
    return material.baseColorFactor.w < material.alphaCutoff;
  }

  const uint3  indices  = getTriangleIndices(mesh.gltfBuffer, mesh.triMesh, primitiveIndex);
  const float2 texCoord = getTriangleAttribute<float2>(mesh.gltfBuffer, mesh.triMesh.texCoords, indices, barycentrics);

  return ResolveBaseColor(material, texCoord).w < material.alphaCutoff;
}

// Visibility and surface resolution
// Shadow-ray visibility queries and the per-hit decode into SurfaceData, plus explicit environment light sampling at a hit.

// Occlusion test along a ray from an already offset origin. Returns true when nothing blocks the segment.
// The shadow path only needs an occlusion answer, so it skips closest-hit and uses a dedicated miss shader to report visibility.
bool TraceVisibilityFromOrigin(float3 rayOrigin, float3 direction, float maxDistance)
{
  ShadowPayload shadowPayload;
  shadowPayload.visible = 0;

  RayDesc ray;
  ray.Origin    = rayOrigin;
  ray.Direction = direction;
  ray.TMin      = 0.001;
  ray.TMax      = maxDistance;

  // Hit group offset 1 and miss index 1 select the shadow any-hit (alpha test) and shadow miss shaders. Any accepted hit ends the search.
  TraceRay(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 1, 0, 1, ray, shadowPayload);

  return shadowPayload.visible != 0;
}

// Convenience wrapper for the common "start from surface point with proper offset" case.
bool TraceVisibility(float3 worldPosition, float3 geometricNormal, float3 direction, float maxDistance)
{
  return TraceVisibilityFromOrigin(OffsetRay(worldPosition, SelectOffsetNormal(geometricNormal, direction)), direction, maxDistance);
}

// Unbounded visibility, for lights at infinity such as the environment.
bool TraceVisibility(float3 worldPosition, float3 geometricNormal, float3 direction)
{
  return TraceVisibility(worldPosition, geometricNormal, direction, kRayTMax);
}

// This is the main scene-data decode step that turns raw glTF buffers and textures into one resolved shading record.
SurfaceData LoadSurfaceData(BuiltInTriangleIntersectionAttributes attr)
{
  // Scene records
  // DXR reports only the second and third barycentric weights; the first is recovered from them.

  const float3 barycentrics   = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
  const uint   instanceIndex  = InstanceIndex();
  const uint   primitiveIndex = PrimitiveIndex();

  const GltfSceneInfo         sceneInfo = pushConst.sceneInfoAddress.Get();
  const GltfInstance          instance  = LoadDeviceArrayElement<GltfInstance>(sceneInfo.instances, instanceIndex);
  const GltfMesh              mesh      = LoadDeviceArrayElement<GltfMesh>(sceneInfo.meshes, instance.meshIndex);
  const GltfMetallicRoughness material  = LoadDeviceArrayElement<GltfMetallicRoughness>(sceneInfo.materials, instance.materialIndex);

  // Vertex attributes
  // Meshes without normals fall back to the object-space face normal, and meshes without texture coordinates use (0, 0).

  const uint3 indices = getTriangleIndices(mesh.gltfBuffer, mesh.triMesh, primitiveIndex);

  const float3 position0 = getAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, indices.x);
  const float3 position1 = getAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, indices.y);
  const float3 position2 = getAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, indices.z);

  const float3 position = getTriangleAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, indices, barycentrics);
  const float3 normal   = mesh.triMesh.normals.count > 0 ? getTriangleAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.normals, indices, barycentrics) : normalize(cross(position1 - position0, position2 - position0));
  const float2 texCoord = mesh.triMesh.texCoords.count > 0 ? getTriangleAttribute<float2>(mesh.gltfBuffer, mesh.triMesh.texCoords, indices, barycentrics) : (float2)0.0;

  // World-space frame
  // The geometric normal comes from world-space vertices, and front-facing is judged from its winding against the ray.
  // Normals transform by the inverse transpose, which is what multiplying by WorldToObject from the left does.

  const float3 worldPosition  = mul(float4(position, 1.0), ObjectToWorld4x3()).xyz;
  const float3 worldPosition0 = mul(float4(position0, 1.0), ObjectToWorld4x3()).xyz;
  const float3 worldPosition1 = mul(float4(position1, 1.0), ObjectToWorld4x3()).xyz;
  const float3 worldPosition2 = mul(float4(position2, 1.0), ObjectToWorld4x3()).xyz;

  const float3 rawGeometricNormal = normalize(cross(worldPosition1 - worldPosition0, worldPosition2 - worldPosition0));
  const uint   isFrontFace        = dot(rawGeometricNormal, WorldRayDirection()) < 0.0 ? 1u : 0u;

  float3 geometricNormal = rawGeometricNormal;
  float3 shadingNormal   = normalize(mul(WorldToObject4x3(), normal).xyz);

  // Keep the shading frame on the same hemisphere as the geometric frame to avoid negative-energy shading artifacts.
  if(dot(shadingNormal, geometricNormal) < 0.0)
  {
    shadingNormal = -shadingNormal;
  }

  // Face-forward both normals against the incoming ray so the rest of the tracer can assume an outward-facing frame.
  if(dot(geometricNormal, WorldRayDirection()) > 0.0)
  {
    geometricNormal = -geometricNormal;
    shadingNormal   = -shadingNormal;
  }

  float3 tangent;
  float3 bitangent;

  BuildSurfaceBasis(mesh, indices, barycentrics, shadingNormal, tangent, bitangent);

  // Normal mapping
  // Normal mapping only runs when a tangent frame exists, because this path expects tangent-space normals.
  // Afterwards the tangent is re-orthogonalized against the mapped normal, and the bitangent keeps the handedness it had before.

  if(material.normalTextureIndex >= 0 && mesh.triMesh.tangents.count > 0)
  {
    shadingNormal = SampleNormalMap(material.normalTextureIndex, texCoord, shadingNormal, tangent, bitangent);
    tangent       = tangent - shadingNormal * dot(shadingNormal, tangent);

    if(dot(tangent, tangent) > 1.0e-6)
    {
      tangent   = normalize(tangent);
      bitangent = normalize(cross(shadingNormal, tangent) * sign(dot(cross(shadingNormal, tangent), bitangent)));
    }
    else
    {
      // If the mapped normal collapses the tangent frame, rebuild a safe orthonormal basis.
      BuildOrthonormalBasis(shadingNormal, tangent, bitangent);
    }
  }

  // Material parameters
  // Texture channels follow the glTF extensions: specular strength in A, transmission and clearcoat in R, thickness and clearcoat roughness in G, sheen roughness in A.

  const float4 baseColor = ResolveBaseColor(material, texCoord);

  const float2 metallicRoughness  = ResolveMetallicRoughness(material, texCoord);
  const float  specular           = clamp(ResolveTextureChannel(material.specularTextureIndex, texCoord, 3, material.specularFactor), 0.0, 1.0);
  const float  specularTint       = clamp(ResolveTintMagnitude(material.specularColorTextureIndex, texCoord, material.specularTint), 0.0, 1.0);
  const float  transmission       = clamp(ResolveTextureChannel(material.transmissionTextureIndex, texCoord, 0, material.transmissionFactor), 0.0, 1.0);
  const float  volumeThickness    = max(ResolveTextureChannel(material.thicknessTextureIndex, texCoord, 1, material.volumeThickness), 0.0);
  const float  clearcoat          = clamp(ResolveTextureChannel(material.clearcoatTextureIndex, texCoord, 0, material.clearcoatFactor), 0.0, 1.0);
  const float  clearcoatRoughness = clamp(ResolveTextureChannel(material.clearcoatRoughnessTextureIndex, texCoord, 1, material.clearcoatRoughness), 0.045, 1.0);
  const float3 sheenColor         = clamp(ResolveTextureColor(material.sheenColorTextureIndex, texCoord, material.sheenColorFactor), (float3)0.0, (float3)1.0);
  const float sheenRoughness      = clamp(ResolveTextureChannel(material.sheenRoughnessTextureIndex, texCoord, 3, material.sheenRoughnessFactor), 0.0, 1.0);

  // Surface record
  // Every field below is the resolved shader-side contract consumed by Disney.hlsli and the direct-light samplers.

  SurfaceData surface;
  surface.worldPosition       = worldPosition;
  surface.shadingNormal       = shadingNormal;
  surface.geometricNormal     = geometricNormal;
  surface.tangent             = tangent;
  surface.bitangent           = bitangent;
  surface.texCoord            = texCoord;
  surface.albedo              = max(baseColor.xyz, (float3)0.0);
  surface.emission            = ResolveEmission(material, texCoord);
  surface.metallic            = metallicRoughness.x;
  surface.roughness           = clamp(metallicRoughness.y, 0.045, 1.0);
  surface.specular            = specular;
  surface.specularTint        = specularTint;
  surface.subsurface          = clamp(material.subsurfaceFactor, 0.0, 1.0);
  surface.anisotropy          = clamp(material.anisotropy, 0.0, 1.0);
  surface.attenuationColor    = clamp(material.attenuationColor, (float3)0.0, (float3)1.0);
  surface.transmission        = transmission;
  surface.attenuationDistance = max(material.attenuationDistance, 0.0);
  surface.volumeThickness     = volumeThickness;
  surface.refractionIndex     = max(material.refractionIndex, 1.01);
  surface.clearcoat           = clearcoat;
  surface.clearcoatRoughness  = clearcoatRoughness;
  surface.sheenColor          = sheenColor;
  surface.sheenRoughness      = sheenRoughness;
  surface.isFrontFace         = isFrontFace;

  return surface;
}

// Draws an environment light direction and its radiance. The sampling PDF is discarded because the caller re-evaluates it with EvaluateEnvironmentLightPdf.
bool SampleEnvironmentLightDirection(GltfSceneInfo sceneInfo, float3 receiverNormal, inout PathSampleStream seed, out float3 lightDir, out float3 radiance)
{
  radiance = (float3)0.0;
  lightDir = (float3)0.0;

  float baseLightPdf = 0.0;

  return SampleEnvironmentBaseLight(sceneInfo, receiverNormal, seed, lightDir, radiance, baseLightPdf);
}

// One environment next-event-estimation sample at the current closest hit.
void AccumulateEnvironmentDirectLight(inout PathPayload payload, SurfaceData surface, GltfSceneInfo sceneInfo, float3 viewDir)
{
  // Eligibility
  // Explicit environment lighting estimates only the broad cosine-proposal BSDF group on opaque surfaces.
  // Glossy reflection / transmission groups are left to continuation rays and miss-side environment hits.
  // A transmission-exit sample would be taken around the flipped normal; CanSampleEnvironmentTransmissionExitLight currently never allows it.

  bool   sampleReflectionNee       = CanSampleEnvironmentReflectionDirectLight(surface);
  bool   sampleTransmissionExitNee = CanSampleEnvironmentTransmissionExitLight(surface);
  float3 receiverNormal            = surface.shadingNormal;

  if(sampleTransmissionExitNee)
  {
    receiverNormal = -surface.shadingNormal;
  }

  if(!sampleReflectionNee && !sampleTransmissionExitNee)
  {
    return;
  }

  // Light sample

  float3 lightDir    = (float3)0.0;
  float3 radiance    = (float3)0.0;

  if(!SampleEnvironmentLightDirection(sceneInfo, receiverNormal, payload.seed, lightDir, radiance))
  {
    return;
  }

  if(!TraceVisibility(surface.worldPosition, surface.geometricNormal, lightDir))
  {
    return;
  }

  float  bsdfPdf = 0.0;
  float3 bsdf    = EvaluateDirectLightBsdf(surface, viewDir, lightDir, bsdfPdf);

  if(bsdfPdf <= 0.0 || SafeMax3(bsdf) <= 0.0)
  {
    return;
  }

  const float lightPdf = EvaluateEnvironmentLightPdf(sceneInfo, receiverNormal, lightDir);

  if(lightPdf <= 0.0)
  {
    return;
  }

  // MIS and accumulation
  // MIS balances the explicit environment sample against the chance of the path continuation BSDF having sampled the same direction.
  // The environment is infinitely far, so the denoiser fallback distance is the far-hit constant.

  const float  misWeight    = MisMixWeight(lightPdf, bsdfPdf);
  const float3 contribution = payload.throughput * radiance * bsdf * (misWeight / lightPdf);

  AccumulatePathContribution(payload, contribution);
  StoreDiffuseDenoiserHitDistanceIfMissing(payload, kDenoiserFarHitDistance);
}

#endif
