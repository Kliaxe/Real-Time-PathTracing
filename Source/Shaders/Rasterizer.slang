/*
 * Copyright (c) 2023-2026, NVIDIA CORPORATION.  All rights reserved.
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
 *
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nvshaders/slang_types.h>
#include "nvshaders/sky_functions.h.slang"

// Shared BSDF helpers (lives under Source/ShaderIncludes and is copied to runtime for hot reload).
#include "ShaderIncludes/Pbr.h.slang"
#include "ShaderIncludes/Camera.h.slang"

// Per-app IO (push constants, bindings, scene structures).
#include "ShaderIo.h"

// clang-format off
[[vk::push_constant]]                       ConstantBuffer<TutoPushConstant> pushConst;
[[vk::binding(BindingPoints::eTextures)]]   Sampler2D textures[];
// clang-format on

// Per-vertex attributes to be assembled from bound vertex buffers.
struct VSin
{
  float3 position : POSITION;
  float3 normal : NORMAl;
};

// Output of the vertex shader.
struct VSout
{
  float4 sv_position : SV_Position;
  float3 worldPos : POSITION;
  float3 worldNormal : NORMAL;
  float2 worldTexCoord : TEXCOORD0;
};

// Output of the fragment shader.
struct PSout
{
  float4 color : SV_Target;
};

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
  // Keep the same environment selection logic as the path tracer so both
  // renderers answer the same question when they need incoming light.
  if(sceneInfo.useSky != 0)
  {
    return evalSimpleSky(sceneInfo.skySimpleParam, direction);
  }

  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0)
  {
    const float2 uv = dirToLatLongUv(direction);
    return textures[sceneInfo.environmentTextureIndex].Sample(uv).xyz;
  }

  return sceneInfo.backgroundColor;
}

float3 EvaluateEnvironmentMaterial(GltfSceneInfo sceneInfo, float3 albedo, float metallic, float roughness, float3 normal, float3 viewDir)
{
  // Keep the raster path readable: a soft diffuse base from the albedo, plus a
  // real environment-map reflection that becomes stronger at grazing angles via
  // Fresnel. This gives us honest view-dependent reflections without pretending
  // we already have a full image-based-lighting pipeline.
  const float3 ambientColor = lerp(float3(0.08, 0.08, 0.08), sceneInfo.backgroundColor, 0.35);
  const float3 diffuse      = albedo * ambientColor * (1.0 - metallic);

  const float  clampedRoughness     = clamp(roughness, 0.0, 1.0);
  const float3 reflectionDir        = reflect(-viewDir, normal);
  const float3 glossyReflectionDir  = normalize(lerp(reflectionDir, normal, clampedRoughness * clampedRoughness));
  const float3 envReflection        = SampleEnvironment(sceneInfo, glossyReflectionDir);

  const float  cMinReflectance = 0.04;
  const float3 f0              = lerp(float3(cMinReflectance), albedo, metallic);
  const float  nDotV           = clampedDot(normal, viewDir);
  const float3 fresnel         = schlickFresnel(f0, float3(1.0), nDotV);

  // Proper glossy IBL would sample a prefiltered environment map by mip level.
  // Until we add that, we approximate roughness in two simple ways:
  // 1. bend rough reflections back toward the surface normal
  // 2. attenuate their intensity so rough dielectrics stop reading as mirrors
  const float  reflectionStrength = lerp(1.0, 0.08, clampedRoughness * clampedRoughness);
  const float3 specular           = envReflection * fresnel * reflectionStrength;

  return diffuse + specular;
}
float2 ResolveMetallicRoughness(GltfMetallicRoughness material, float2 texCoord)
{
  // glTF stores perceptual roughness in G and metallic in B for the combined
  // metallic-roughness texture. When no texture is present, the scalar factors
  // remain the material-wide fallback.
  float metallic  = material.metallicFactor;
  float roughness = material.roughnessFactor;

  if(material.metallicRoughnessTextureIndex > -1)
  {
    const float4 metallicRoughnessSample = textures[material.metallicRoughnessTextureIndex].Sample(texCoord);
    roughness *= metallicRoughnessSample.g;
    metallic *= metallicRoughnessSample.b;
  }

  return clamp(float2(metallic, roughness), 0.0, 1.0);
}

__generic<T : IFloat> T getAttribute(uint8_t* dataBufferAddress, BufferView bufferView, uint attributeIndex)
{
  if(bufferView.count > 0)
  {
    T* ptr = (T*)(dataBufferAddress + bufferView.offset + attributeIndex * bufferView.byteStride);
    return ptr[0];
  }

  return T(1);  // Error case
}

// Vertex shader.
[shader("vertex")]
VSout vertexMain(VSin input, uint vertexIndex : SV_VertexID)
{
  int instanceIndex = pushConst.instanceIndex;

  GltfSceneInfo* sceneInfo = pushConst.sceneInfoAddress;

  if(instanceIndex < 0)
  {
    const float2 kFullscreenPos[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    float2 ndc = kFullscreenPos[vertexIndex];

    VSout output;
    output.sv_position   = float4(ndc, 0.0, 1.0);
    output.worldPos      = float3(0.0);
    output.worldNormal   = float3(0.0, 1.0, 0.0);
    output.worldTexCoord = float2(0.0);
    return output;
  }

  GltfInstance instance = sceneInfo.instances[instanceIndex];
  GltfMesh     meshIo   = sceneInfo.meshes[instance.meshIndex];

  // Pull vertex attributes directly from GPU buffers.
  float3 posMesh  = getAttribute<float3>(meshIo.gltfBuffer, meshIo.triMesh.positions, vertexIndex);
  float3 normal   = getAttribute<float3>(meshIo.gltfBuffer, meshIo.triMesh.normals, vertexIndex);
  float2 texCoord = getAttribute<float2>(meshIo.gltfBuffer, meshIo.triMesh.texCoords, vertexIndex);

  float4 pos = mul(float4(posMesh, 1.0), instance.transform);

  VSout output;
  output.sv_position   = mul(pos, sceneInfo.viewProjMatrix);
  output.worldPos      = pos.xyz;
  output.worldNormal   = normalize(mul(normal, float3x3(pushConst.normalMatrix)));
  output.worldTexCoord = texCoord;

  return output;
}

// Fragment shader.
[shader("pixel")]
PSout fragmentMain(VSout stage)
{
  GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  if(pushConst.instanceIndex < 0)
  {
    // Use the same camera reconstruction helper as the path tracer so the
    // raster background and ray traced camera rays stay perfectly aligned.
    float3 worldDir = ReconstructWorldDirectionFromPixel(stage.sv_position.xy, sceneInfo);
    float3 color    = SampleEnvironment(sceneInfo, worldDir);

    PSout output;
    output.color = float4(color, 1.0);
    return output;
  }

  GltfInstance          instance = sceneInfo.instances[pushConst.instanceIndex];
  GltfMetallicRoughness material = sceneInfo.materials[instance.materialIndex];
  float3                N        = normalize(stage.worldNormal);
  float3                V        = normalize(sceneInfo.cameraPosition - stage.worldPos);

  // Base color from factor and optional texture.
  float4 baseColor = material.baseColorFactor;
  if(material.baseColorTextureIndex > -1)
  {
    baseColor *= textures[material.baseColorTextureIndex].Sample(stage.worldTexCoord);
  }

  if(material.alphaMode == GltfAlphaMode::eMask && baseColor.w < material.alphaCutoff)
  {
    discard;
  }
  float3 albedo = baseColor.xyz;

  // Metallic and roughness still matter here, but only for how strongly the
  // environment reflection shows up. The diffuse base remains intentionally
  // simple so the shader stays easy to read while we learn Vulkan plumbing.
  float2 metallicRoughness = ResolveMetallicRoughness(material, stage.worldTexCoord);
  float  metallic          = metallicRoughness.x;
  float  roughness         = metallicRoughness.y;
  if(pushConst.metallicRoughnessOverride.x >= 0.0)
  {
    metallic = pushConst.metallicRoughnessOverride.x;
  }
  if(pushConst.metallicRoughnessOverride.y >= 0.0)
  {
    roughness = pushConst.metallicRoughnessOverride.y;
  }

  const float3 color = EvaluateEnvironmentMaterial(sceneInfo, albedo, metallic, roughness, N, V);

  PSout output;
  output.color = float4(max(color, float3(0.0)), 1.0);
  return output;
}










