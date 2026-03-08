/*
 * Copyright (c) 2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "nvshaders/sky_functions.h.slang"
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIo.h"

// clang-format off
[[vk::push_constant]]                              ConstantBuffer<PathTracePushConstant> pushConst;
[[vk::binding(BindingPoints::eTextures)]]          Sampler2D textures[];
[[vk::binding(BindingPoints::eTlas)]]              RaytracingAccelerationStructure topLevelAS;
[[vk::binding(BindingPoints::eOutputImage)]]       RWTexture2D<float4> outImage;
[[vk::binding(BindingPoints::eAccumulationImage)]] RWTexture2D<float4> accumulationImage;
// clang-format on

// Slang does not provide a built-in symbolic "infinite" ray distance here, so
// we use a very large finite value for "trace until the scene or miss shader
// says otherwise". This keeps the ray setup explicit for learning purposes.
static const float kRayTMax = 1.0e32;
static const uint  kPathTraceFlagAccumulate = 0x1u;

struct PathPayload
{
  float3 radiance;
  float3 throughput;
  uint   seed;
  uint   depth;
};

bool IsAccumulationEnabled()
{
  return (pushConst.flags & kPathTraceFlagAccumulate) != 0;
}

float2 dirToLatLongUv(float3 dir)
{
  const float kInvPi    = 0.31830988618;
  const float kInvTwoPi = 0.15915494309;

  float3 n = normalize(dir);
  float  u = 0.5 - atan2(n.z, n.x) * kInvTwoPi;
  float  v = 0.5 - asin(clamp(n.y, -1.0, 1.0)) * kInvPi;
  return float2(u, v);
}

float3 SampleEnvironment(GltfSceneInfo sceneInfo, float3 direction)
{
  // Match the scene-level environment selection used elsewhere in the sample:
  // procedural sky first, then HDRI, then the plain fallback background color.
  if(sceneInfo.useSky != 0)
  {
    return evalSimpleSky(sceneInfo.skySimpleParam, direction);
  }

  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0)
  {
    const float2 uv = dirToLatLongUv(direction);
    return textures[sceneInfo.environmentTextureIndex].SampleLevel(uv, 0.0).xyz;
  }

  return sceneInfo.backgroundColor;
}

float NextRandom(inout uint seed)
{
  return rand(seed);
}

float3 BuildTangent(float3 normal)
{
  const float3 up = (abs(normal.z) < 0.999) ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
  return normalize(cross(up, normal));
}

float3 SampleCosineHemisphere(float3 normal, inout uint seed)
{
  const float2 xi = float2(NextRandom(seed), NextRandom(seed));

  const float  r        = sqrt(xi.x);
  const float  phi      = 6.28318530718 * xi.y;
  const float3 localDir = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - xi.x)));

  const float3 tangent   = BuildTangent(normal);
  const float3 bitangent = cross(normal, tangent);

  return normalize(localDir.x * tangent + localDir.y * bitangent + localDir.z * normal);
}

__generic<T : IFloat> T getAttribute(uint8_t* dataBufferAddress, BufferView bufferView, uint attributeIndex)
{
  if(bufferView.count > 0)
  {
    T* ptr = (T*)(dataBufferAddress + bufferView.offset + attributeIndex * bufferView.byteStride);
    return ptr[0];
  }

  return T(1);
}

int3 getTriangleIndices(uint8_t* dataBufferAddress, const TriangleMesh mesh, int primitiveID)
{
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
  T attr0 = getAttribute<T>(dataBufferAddress, bufferView, attributeIndex.x);
  T attr1 = getAttribute<T>(dataBufferAddress, bufferView, attributeIndex.y);
  T attr2 = getAttribute<T>(dataBufferAddress, bufferView, attributeIndex.z);
  return T(barycentrics.x) * attr0 + T(barycentrics.y) * attr1 + T(barycentrics.z) * attr2;
}

[shader("raygeneration")]
void rgenMain()
{
  const uint2 launchID = DispatchRaysIndex().xy;

  GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];

  // Match the raster sky/background path exactly: both rendering modes rebuild
  // camera rays from the same shared scene buffer data and helper function.
  const float2 pixelCenter = float2(launchID) + 0.5;

  RayDesc ray;
  ray.Origin    = sceneInfo.cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  PathPayload payload;
  payload.radiance   = float3(0.0, 0.0, 0.0);
  payload.throughput = float3(1.0, 1.0, 1.0);
  payload.seed       = xxhash32(uint3(launchID, pushConst.rngFrameNumber));
  payload.depth      = 0;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, payload);

  const float3 sampleRadiance = max(payload.radiance, float3(0.0, 0.0, 0.0));
  float3       resolvedRadiance = sampleRadiance;

  if(IsAccumulationEnabled() && pushConst.accumulatedFrames > 0)
  {
    const float3 previousAverage = accumulationImage[int2(launchID)].xyz;
    const float  historyWeight   = float(pushConst.accumulatedFrames);
    resolvedRadiance = (previousAverage * historyWeight + sampleRadiance) / (historyWeight + 1.0);
  }

  accumulationImage[int2(launchID)] = float4(resolvedRadiance, 1.0);
  outImage[int2(launchID)]          = float4(resolvedRadiance, 1.0);
}

[shader("miss")]
void rmissMain(inout PathPayload payload)
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const float3        envColor  = SampleEnvironment(sceneInfo, WorldRayDirection());
  payload.radiance += payload.throughput * envColor;
}

[shader("closesthit")]
void rchitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  const float3 barycentrics   = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
  const uint   instanceIndex  = InstanceIndex();
  const uint   primitiveIndex = PrimitiveIndex();

  const GltfSceneInfo         sceneInfo = pushConst.sceneInfoAddress[0];
  const GltfInstance          instance  = sceneInfo.instances[instanceIndex];
  const GltfMesh              mesh      = sceneInfo.meshes[instance.meshIndex];
  const GltfMetallicRoughness material  = sceneInfo.materials[instance.materialIndex];

  const int3   indices  = getTriangleIndices(mesh.gltfBuffer, mesh.triMesh, primitiveIndex);
  const float3 pos      = getTriangleAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.positions, indices, barycentrics);
  const float3 nrm      = getTriangleAttribute<float3>(mesh.gltfBuffer, mesh.triMesh.normals, indices, barycentrics);
  const float2 texCoord = getTriangleAttribute<float2>(mesh.gltfBuffer, mesh.triMesh.texCoords, indices, barycentrics);

  const float3 worldPos = mul(float4(pos, 1.0), ObjectToWorld4x3()).xyz;
  float3       worldNormal = normalize(mul(WorldToObject4x3(), nrm).xyz);

  if(dot(worldNormal, WorldRayDirection()) > 0.0)
  {
    worldNormal = -worldNormal;
  }

  float4 baseColor = material.baseColorFactor;
  if(material.baseColorTextureIndex >= 0)
  {
    baseColor *= textures[material.baseColorTextureIndex].SampleLevel(texCoord, 0.0);
  }

  const float3 albedo = max(baseColor.xyz, float3(0.0, 0.0, 0.0));

  if(payload.depth >= pushConst.maxBounces)
  {
    return;
  }

  payload.depth += 1;
  payload.throughput *= albedo;

  RayDesc bounceRay;
  bounceRay.Origin    = worldPos + worldNormal * 0.001;
  bounceRay.Direction = SampleCosineHemisphere(worldNormal, payload.seed);
  bounceRay.TMin      = 0.001;
  bounceRay.TMax      = kRayTMax;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
}


