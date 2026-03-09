#ifndef PATH_TRACING_INTERSECTION_H
#define PATH_TRACING_INTERSECTION_H

#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"

// -------------------------------------------------------------------------
//    Geometry access
// -------------------------------------------------------------------------

float2 ResolveMetallicRoughness(GltfMetallicRoughness material, float2 texCoord)
{
  // glTF packs roughness in G and metallic in B, with scalar factors acting as post-multipliers.
  float metallic  = material.metallicFactor;
  float roughness = material.roughnessFactor;

  if(material.metallicRoughnessTextureIndex > -1)
  {
    const float4 metallicRoughnessSample = textures[material.metallicRoughnessTextureIndex].SampleLevel(texCoord, 0.0);
    roughness *= metallicRoughnessSample.g;
    metallic *= metallicRoughnessSample.b;
  }

  return clamp(float2(metallic, roughness), 0.0, 1.0);
}

float ResolveTextureChannel(int textureIndex, float2 texCoord, int channel, float fallback)
{
  // Small helper for extension textures that store one scalar in one known channel.
  if(textureIndex < 0)
  {
    return fallback;
  }

  const float4 sample = textures[textureIndex].SampleLevel(texCoord, 0.0);
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

float3 ResolveTextureColor(int textureIndex, float2 texCoord, float3 fallback)
{
  // Small helper for extension textures that modulate an RGB factor directly.
  if(textureIndex < 0)
  {
    return fallback;
  }

  return fallback * textures[textureIndex].SampleLevel(texCoord, 0.0).xyz;
}

float ResolveTintMagnitude(int textureIndex, float2 texCoord, float fallback)
{
  // Interprets a tint texture as a color magnitude control and collapses it back to a scalar tint amount.
  if(textureIndex < 0)
  {
    return fallback;
  }

  const float3 sample = textures[textureIndex].SampleLevel(texCoord, 0.0).xyz;
  return fallback * length(sample);
}

float3 ResolveEmission(GltfMetallicRoughness material, float2 texCoord)
{
  // glTF emissive texture modulates the emissive factor instead of replacing it.
  float3 emission = max(material.emissionFactor, float3(0.0));
  if(material.emissiveTextureIndex >= 0)
  {
    emission *= textures[material.emissiveTextureIndex].SampleLevel(texCoord, 0.0).xyz;
  }
  return max(emission, float3(0.0));
}

float4 ResolveBaseColor(GltfMetallicRoughness material, float2 texCoord)
{
  // Base color stays in RGBA because alpha masking still depends on the A channel.
  float4 baseColor = material.baseColorFactor;
  if(material.baseColorTextureIndex >= 0)
  {
    baseColor *= textures[material.baseColorTextureIndex].SampleLevel(texCoord, 0.0);
  }
  return baseColor;
}

__generic<T : IFloat> T getAttribute(uint8_t* dataBufferAddress, BufferView bufferView, uint attributeIndex)
{
  // The raw glTF buffer is already on the GPU, so hit shaders fetch attributes directly through the uploaded byte layout.
  if(bufferView.count > 0)
  {
    T* ptr = (T*)(dataBufferAddress + bufferView.offset + attributeIndex * bufferView.byteStride);
    return ptr[0];
  }

  return T(1);
}

int3 getTriangleIndices(uint8_t* dataBufferAddress, const TriangleMesh mesh, int primitiveID)
{
  // Supports both 16-bit and 32-bit index streams from imported glTF meshes.
  if(mesh.indices.byteStride == sizeof(int16_t))
  {
    int16_t3* indices = (int16_t3*)(dataBufferAddress + mesh.indices.offset);
    return indices[primitiveID];
  }
  else if(mesh.indices.byteStride == sizeof(int32_t))
  {
    int3* indices = (int3*)(dataBufferAddress + mesh.indices.offset);
    return indices[primitiveID];
  }

  return int3(-1);
}

__generic<T : IFloat> T getTriangleAttribute(uint8_t* dataBufferAddress, BufferView bufferView, uint3 attributeIndex, float3 barycentrics)
{
  // Hit attributes are reconstructed by barycentric interpolation in object-space vertex order.
  const T attr0 = getAttribute<T>(dataBufferAddress, bufferView, attributeIndex.x);
  const T attr1 = getAttribute<T>(dataBufferAddress, bufferView, attributeIndex.y);
  const T attr2 = getAttribute<T>(dataBufferAddress, bufferView, attributeIndex.z);
  return T(barycentrics.x) * attr0 + T(barycentrics.y) * attr1 + T(barycentrics.z) * attr2;
}

void BuildSurfaceBasis(GltfMesh mesh, int3 indices, float3 barycentrics, float3 shadingNormal, out float3 tangent, out float3 bitangent)
{
  // If the asset has no tangent frame, build one procedurally so anisotropy / normal mapping still have a stable basis.
  if(mesh.triMesh.tangents.count == 0)
  {
    orthonormalBasis(shadingNormal, tangent, bitangent);
    return;
  }

  const float4 tangentSample = getTriangleAttribute<float4>(mesh.gltfBuffer, mesh.triMesh.tangents, uint3(indices), barycentrics);
  tangent                     = normalize(mul(float4(tangentSample.xyz, 0.0), ObjectToWorld4x3()));
  tangent                     = tangent - shadingNormal * dot(shadingNormal, tangent);

  if(dot(tangent, tangent) <= 1.0e-6)
  {
    // Degenerate authored tangents are treated like missing tangents.
    orthonormalBasis(shadingNormal, tangent, bitangent);
    return;
  }

  tangent   = normalize(tangent);
  bitangent = normalize(cross(shadingNormal, tangent) * tangentSample.w);
}

float3 SampleNormalMap(int textureIndex, float2 texCoord, float3 shadingNormal, float3 tangent, float3 bitangent)
{
  // Tangent-space normal maps are decoded from [0, 1] into [-1, 1] before world-space reconstruction.
  const float3 normalSample = textures[textureIndex].SampleLevel(texCoord, 0.0).xyz * 2.0 - 1.0;
  const float3 mappedNormal = tangent * normalSample.x + bitangent * normalSample.y + shadingNormal * normalSample.z;
  return normalize(mappedNormal);
}

bool IsMaskedSurfaceHit(BuiltInTriangleIntersectionAttributes attr)
{
  // The any-hit shaders use this to punch holes in cutout materials without treating blend materials as stochastic transparency.
  const float3 barycentrics  = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
  const uint   instanceIndex = InstanceIndex();
  const uint   primitiveIndex = PrimitiveIndex();

  const GltfSceneInfo         sceneInfo = pushConst.sceneInfoAddress[0];
  const GltfInstance          instance  = sceneInfo.instances[instanceIndex];
  const GltfMesh              mesh      = sceneInfo.meshes[instance.meshIndex];
  const GltfMetallicRoughness material  = sceneInfo.materials[instance.materialIndex];

  if(material.alphaMode != GltfAlphaMode::eMask)
  {
    return false;
  }

  if(mesh.triMesh.texCoords.count == 0)
  {
    return material.baseColorFactor.w < material.alphaCutoff;
  }

  const int3   indices  = getTriangleIndices(mesh.gltfBuffer, mesh.triMesh, primitiveIndex);
  const float2 texCoord = getTriangleAttribute<float2>(mesh.gltfBuffer, mesh.triMesh.texCoords, uint3(indices), barycentrics);
  return ResolveBaseColor(material, texCoord).w < material.alphaCutoff;
}

// -------------------------------------------------------------------------
//    Visibility and surface resolution
// -------------------------------------------------------------------------

bool TraceVisibilityFromOrigin(float3 rayOrigin, float3 direction, float maxDistance)
{
  // The shadow path only needs an occlusion answer, so it skips closest-hit and uses a dedicated miss shader to report visibility.
  ShadowPayload shadowPayload;
  shadowPayload.visible = 0;

  RayDesc ray;
  ray.Origin    = rayOrigin;
  ray.Direction = direction;
  ray.TMin      = 0.001;
  ray.TMax      = maxDistance;

  TraceRay(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 1, 0, 1, ray,
           shadowPayload);

  return shadowPayload.visible != 0;
}

bool TraceVisibility(float3 worldPosition, float3 geometricNormal, float3 direction, float maxDistance)
{
  // Convenience wrapper for the common "start from surface point with proper offset" case.
  return TraceVisibilityFromOrigin(OffsetRay(worldPosition, SelectOffsetNormal(geometricNormal, direction)), direction, maxDistance);
}

bool TraceVisibility(float3 worldPosition, float3 geometricNormal, float3 direction)
{
  return TraceVisibility(worldPosition, geometricNormal, direction, kRayTMax);
}

SurfaceData LoadSurfaceData(BuiltInTriangleIntersectionAttributes attr)
{
  // This is the main scene-data decode step that turns raw glTF buffers and textures into one resolved shading record.
  const float3 barycentrics  = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
  const uint   instanceIndex = InstanceIndex();
  const uint   primitiveIndex = PrimitiveIndex();

  const GltfSceneInfo         sceneInfo = pushConst.sceneInfoAddress[0];
  const GltfInstance          instance  = sceneInfo.instances[instanceIndex];
  const GltfMesh              mesh      = sceneInfo.meshes[instance.meshIndex];
  const GltfMetallicRoughness material  = sceneInfo.materials[instance.materialIndex];

  const int3 indices = getTriangleIndices(mesh.gltfBuffer, mesh.triMesh, primitiveIndex);

  const float3 position0 = getAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, indices.x);
  const float3 position1 = getAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, indices.y);
  const float3 position2 = getAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, indices.z);

  const float3 position = getTriangleAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, uint3(indices), barycentrics);
  const float3 normal = mesh.triMesh.normals.count > 0
                            ? getTriangleAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.normals, uint3(indices), barycentrics)
                            : normalize(cross(position1 - position0, position2 - position0));
  const float2 texCoord = mesh.triMesh.texCoords.count > 0
                              ? getTriangleAttribute<float2>(mesh.gltfBuffer, mesh.triMesh.texCoords, uint3(indices), barycentrics)
                              : float2(0.0);

  const float3 worldPosition  = mul(float4(position, 1.0), ObjectToWorld4x3()).xyz;
  const float3 worldPosition0 = mul(float4(position0, 1.0), ObjectToWorld4x3()).xyz;
  const float3 worldPosition1 = mul(float4(position1, 1.0), ObjectToWorld4x3()).xyz;
  const float3 worldPosition2 = mul(float4(position2, 1.0), ObjectToWorld4x3()).xyz;

  const float3 rawGeometricNormal = normalize(cross(worldPosition1 - worldPosition0, worldPosition2 - worldPosition0));
  const uint   isFrontFace        = dot(rawGeometricNormal, WorldRayDirection()) < 0.0 ? 1u : 0u;

  float3 geometricNormal = rawGeometricNormal;
  float3 shadingNormal   = normalize(mul(WorldToObject4x3(), normal).xyz);

  if(dot(shadingNormal, geometricNormal) < 0.0)
  {
    // Keep the shading frame on the same hemisphere as the geometric frame to avoid negative-energy shading artifacts.
    shadingNormal = -shadingNormal;
  }

  if(dot(geometricNormal, WorldRayDirection()) > 0.0)
  {
    // Face-forward both normals against the incoming ray so the rest of the tracer can assume an outward-facing frame.
    geometricNormal = -geometricNormal;
    shadingNormal   = -shadingNormal;
  }

  float3 tangent;
  float3 bitangent;
  BuildSurfaceBasis(mesh, indices, barycentrics, shadingNormal, tangent, bitangent);

  if(material.normalTextureIndex >= 0 && mesh.triMesh.tangents.count > 0)
  {
    // Normal mapping only runs when a tangent frame exists, because this path expects tangent-space normals.
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
      orthonormalBasis(shadingNormal, tangent, bitangent);
    }
  }

  const float4 baseColor = ResolveBaseColor(material, texCoord);

  const float2 metallicRoughness  = ResolveMetallicRoughness(material, texCoord);
  const float  specular           = clamp(ResolveTextureChannel(material.specularTextureIndex, texCoord, 3, material.specularFactor), 0.0, 1.0);
  const float  specularTint       = clamp(ResolveTintMagnitude(material.specularColorTextureIndex, texCoord, material.specularTint), 0.0, 1.0);
  const float  transmission       = clamp(ResolveTextureChannel(material.transmissionTextureIndex, texCoord, 0, material.transmissionFactor), 0.0, 1.0);
  const float  volumeThickness    = max(ResolveTextureChannel(material.thicknessTextureIndex, texCoord, 1, material.volumeThickness), 0.0);
  const float  clearcoat          = clamp(ResolveTextureChannel(material.clearcoatTextureIndex, texCoord, 0, material.clearcoatFactor), 0.0, 1.0);
  const float  clearcoatRoughness = clamp(ResolveTextureChannel(material.clearcoatRoughnessTextureIndex, texCoord, 1, material.clearcoatRoughness), 0.045, 1.0);
  const float3 sheenColor         = clamp(ResolveTextureColor(material.sheenColorTextureIndex, texCoord, material.sheenColorFactor), float3(0.0), float3(1.0));
  const float sheenRoughness      = clamp(ResolveTextureChannel(material.sheenRoughnessTextureIndex, texCoord, 3, material.sheenRoughnessFactor), 0.0, 1.0);

  SurfaceData surface;
  // Every field below is the resolved shader-side contract consumed by Disney.h.slang and the direct-light samplers.
  surface.worldPosition      = worldPosition;
  surface.shadingNormal      = shadingNormal;
  surface.geometricNormal    = geometricNormal;
  surface.tangent            = tangent;
  surface.bitangent          = bitangent;
  surface.texCoord           = texCoord;
  surface.albedo             = max(baseColor.xyz, float3(0.0));
  surface.emission           = ResolveEmission(material, texCoord);
  surface.metallic           = metallicRoughness.x;
  surface.roughness          = clamp(metallicRoughness.y, 0.045, 1.0);
  surface.specular           = specular;
  surface.specularTint       = specularTint;
  surface.subsurface         = clamp(material.subsurfaceFactor, 0.0, 1.0);
  surface.anisotropy         = clamp(material.anisotropy, 0.0, 1.0);
  surface.attenuationColor   = clamp(material.attenuationColor, float3(0.0), float3(1.0));
  surface.transmission       = transmission;
  surface.attenuationDistance = max(material.attenuationDistance, 0.0);
  surface.volumeThickness    = volumeThickness;
  surface.refractionIndex    = max(material.refractionIndex, 1.01);
  surface.clearcoat          = clearcoat;
  surface.clearcoatRoughness = clearcoatRoughness;
  surface.sheenColor         = sheenColor;
  surface.sheenRoughness     = sheenRoughness;
  surface.isFrontFace        = isFrontFace;
  return surface;
}

bool SampleEnvironmentLightDirection(GltfSceneInfo sceneInfo,
                                     float3 receiverNormal,
                                     inout uint seed,
                                     out float3 lightDir,
                                     out float3 radiance)
{
  radiance = float3(0.0);
  lightDir = float3(0.0);

  float baseLightPdf = 0.0;
  return SampleEnvironmentBaseLight(sceneInfo, receiverNormal, seed, lightDir, radiance, baseLightPdf);
}

void AccumulateEnvironmentDirectLight(inout PathPayload payload, SurfaceData surface, GltfSceneInfo sceneInfo, float3 viewDir)
{
  // Explicit environment lighting estimates only the broad cosine-proposal BSDF group on opaque surfaces.
  // Glossy reflection / transmission groups are left to continuation rays and miss-side environment hits.
  bool   sampleReflectionNee = CanSampleEnvironmentReflectionDirectLight(surface);
  bool   sampleTransmissionExitNee = CanSampleEnvironmentTransmissionExitLight(surface);
  float3 receiverNormal      = surface.shadingNormal;

  if(sampleTransmissionExitNee)
  {
    receiverNormal = -surface.shadingNormal;
  }

  if(!sampleReflectionNee && !sampleTransmissionExitNee)
  {
    return;
  }

  float3 lightDir    = float3(0.0);
  float3 radiance    = float3(0.0);
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

  // MIS balances the explicit environment sample against the chance of the path continuation BSDF having sampled the same direction.
  const float  misWeight = MisMixWeight(lightPdf, bsdfPdf);

  payload.radiance += payload.throughput * radiance * bsdf * (misWeight / lightPdf);
}

#endif
