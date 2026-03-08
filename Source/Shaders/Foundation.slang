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

float2 dirToLatLongUv(float3 dir)
{
  const float kInvPi = 0.31830988618;
  const float kInvTwoPi = 0.15915494309;
  float3 n = normalize(dir);
  float  u = 0.5 - atan2(n.z, n.x) * kInvTwoPi;
  float  v = 0.5 - asin(clamp(n.y, -1.0, 1.0)) * kInvPi;
  return float2(u, v);
}

// Output of the fragment shader.
struct PSout
{
  float4 color : SV_Target;
};

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
  GltfSceneInfo         sceneInfo = pushConst.sceneInfoAddress[0];
  if(pushConst.instanceIndex < 0)
  {
    float3 color = sceneInfo.backgroundColor;
    if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0)
    {
      // Use the same camera reconstruction helper as the path tracer so the
      // raster background and ray traced camera rays stay perfectly aligned.
      float3 worldDir = ReconstructWorldDirectionFromPixel(stage.sv_position.xy, sceneInfo);
      float2 envUv      = dirToLatLongUv(worldDir);
      color = textures[sceneInfo.environmentTextureIndex].Sample(envUv).xyz;
    }

    PSout output;
    output.color = float4(color, 1.0);
    return output;
  }

  GltfInstance          instance  = sceneInfo.instances[pushConst.instanceIndex];
  GltfMetallicRoughness material  = sceneInfo.materials[instance.materialIndex];

  GltfPunctual light = sceneInfo.punctualLights[0];  // We use only the first light for now.

  if(sceneInfo.useSky == 1)
  {
    light.direction = sceneInfo.skySimpleParam.sunDirection;
    light.color     = sceneInfo.skySimpleParam.sunColor;
    light.intensity = sceneInfo.skySimpleParam.sunIntensity;
    light.type      = GltfLightType::eDirectional;
  }

  if(light.type == GltfLightType::ePoint)
  {
    light.direction = light.position - stage.worldPos;
    float d         = length(light.direction);
    light.intensity /= (d * d);
  }
  else if(light.type == GltfLightType::eSpot)
  {
    float3 lightDir = light.position - stage.worldPos;
    float  d        = length(lightDir);
    light.intensity /= (d * d);

    float theta         = (dot(normalize(lightDir), normalize(light.direction)));
    float spotIntensity = clamp((theta - cos(light.coneAngle)) / (1.0 - cos(light.coneAngle)), 0.0, 1.0);
    light.intensity *= spotIntensity;
    light.direction = lightDir;
  }

  float3 V = normalize(sceneInfo.cameraPosition - stage.worldPos);
  float3 N = normalize(stage.worldNormal);
  float3 L = normalize(light.direction);

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

  // Metallic/roughness with optional UI override.
  float metallic  = material.metallicFactor;
  float roughness = material.roughnessFactor;
  if(pushConst.metallicRoughnessOverride.x >= 0.0)
  {
    metallic = pushConst.metallicRoughnessOverride.x;
  }
  if(pushConst.metallicRoughnessOverride.y >= 0.0)
  {
    roughness = pushConst.metallicRoughnessOverride.y;
  }

  // Simple PBR lighting.
  float3 color = pbrMetallicRoughness(albedo, metallic, roughness, N, V, L);
  color *= light.color * light.intensity;

  PSout output;
  output.color = float4(max(color, float3(0.0)), 1.0);
  return output;
}


