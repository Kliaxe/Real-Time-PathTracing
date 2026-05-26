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
#include "NRD.hlsli"

[[vk::binding(0, 0)]] Texture2D<float4> inDiffuseRadianceHitDistance;
[[vk::binding(1, 0)]] Texture2D<float4> inSpecularRadianceHitDistance;
[[vk::binding(2, 0)]] Texture2D<float4> inRawBeauty;
[[vk::binding(3, 0)]] Texture2D<float> inViewZ;
[[vk::binding(4, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(5, 0)]] Texture2D<float4> inMotionVectors;
[[vk::binding(6, 0)]] Texture2D<float4> inNoisyDiffuseRadianceHitDistance;
[[vk::binding(7, 0)]] Texture2D<float4> inNoisySpecularRadianceHitDistance;
[[vk::binding(8, 0)]] Texture2D<float4> inBaseColorMetalness;
[[vk::binding(9, 0)]] Texture2D<float4> inSpecularDemodulationFactor;
[[vk::binding(10, 0)]] RWTexture2D<float4> outImage;

static const float kInvalidDenoiserViewZThreshold = 1.0e31;
static const float3 kMinDiffuseDemodulationFactor = float3(0.05, 0.05, 0.05);
static const float3 kMinSpecularDemodulationFactor = float3(0.02, 0.02, 0.02);

struct ResolvePushConstants
{
  uint debugView;
  uint useMaterialDemodulation;
};

[[vk::push_constant]] ConstantBuffer<ResolvePushConstants> pushConstants;

float3 VisualizeNormalRoughness(float4 packedNormalRoughness)
{
  const float4 unpacked = NRD_FrontEnd_UnpackNormalAndRoughness(packedNormalRoughness);
  return 0.5 * unpacked.xyz + 0.5;
}

float3 VisualizeViewZ(float viewZ)
{
  if(viewZ >= kInvalidDenoiserViewZThreshold)
  {
    return float3(0.0);
  }

  const float distance = abs(viewZ);
  const float normalized = saturate(distance / (distance + 10.0));
  return normalized.xxx;
}

float3 VisualizeMotion(float4 motion)
{
  return saturate(float3(motion.xy * 0.5 + 0.5, 0.5));
}

float3 ComputeDiffuseDemodulationFactor(float4 baseColorMetalness)
{
  const float3 diffuseFactor = saturate(baseColorMetalness.rgb) * (1.0 - saturate(baseColorMetalness.a));
  return max(diffuseFactor, kMinDiffuseDemodulationFactor);
}

float3 ComputeSpecularDemodulationFactor(float4 packedSpecularFactor)
{
  // The ray tracing passes computed NRD_MaterialFactors with the exact primary-view vector.
  // Compose only needs to reapply the stored factor after REBLUR has filtered pure specular radiance.
  return max(packedSpecularFactor.rgb, kMinSpecularDemodulationFactor);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
  uint width;
  uint height;
  outImage.GetDimensions(width, height);

  if(dispatchThreadID.x >= width || dispatchThreadID.y >= height)
  {
    return;
  }

  const int3 imagePosition = int3(dispatchThreadID.xy, 0);
  const float4 diffusePacked  = inDiffuseRadianceHitDistance.Load(imagePosition);
  const float4 specularPacked = inSpecularRadianceHitDistance.Load(imagePosition);
  const float4 rawBeauty      = inRawBeauty.Load(imagePosition);
  const float  viewZ          = inViewZ.Load(int3(dispatchThreadID.xy, 0));
  const float4 normalRoughness = inNormalRoughness.Load(imagePosition);
  const float4 motionVectors   = inMotionVectors.Load(imagePosition);
  const float4 noisyDiffusePacked  = inNoisyDiffuseRadianceHitDistance.Load(imagePosition);
  const float4 noisySpecularPacked = inNoisySpecularRadianceHitDistance.Load(imagePosition);
  const float4 baseColorMetalness  = inBaseColorMetalness.Load(imagePosition);
  const float4 specularDemodulationFactorPacked = inSpecularDemodulationFactor.Load(imagePosition);

  const float4 diffuseResolved      = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffusePacked);
  const float4 specularResolved     = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specularPacked);
  const float4 noisyDiffuseResolved = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(noisyDiffusePacked);
  const float4 noisySpecularResolved = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(noisySpecularPacked);
  const float3 diffuseDemodulationFactor = ComputeDiffuseDemodulationFactor(baseColorMetalness);
  const float3 specularDemodulationFactor = ComputeSpecularDemodulationFactor(specularDemodulationFactorPacked);
  const float3 remodulatedDiffuse = (pushConstants.useMaterialDemodulation != 0u)
                                        ? max(diffuseResolved.rgb, float3(0.0)) * diffuseDemodulationFactor
                                        : max(diffuseResolved.rgb, float3(0.0));
  const float3 remodulatedSpecular = (pushConstants.useMaterialDemodulation != 0u)
                                         ? max(specularResolved.rgb, float3(0.0)) * specularDemodulationFactor
                                         : max(specularResolved.rgb, float3(0.0));
  const float3 noisyRemodulatedDiffuse = (pushConstants.useMaterialDemodulation != 0u)
                                             ? max(noisyDiffuseResolved.rgb, float3(0.0)) * diffuseDemodulationFactor
                                             : max(noisyDiffuseResolved.rgb, float3(0.0));
  const float3 noisyRemodulatedSpecular = (pushConstants.useMaterialDemodulation != 0u)
                                              ? max(noisySpecularResolved.rgb, float3(0.0)) * specularDemodulationFactor
                                              : max(noisySpecularResolved.rgb, float3(0.0));
  const float3 combinedRadiance     = remodulatedDiffuse + remodulatedSpecular;

  float3 outputRadiance = combinedRadiance;

  switch(pushConstants.debugView)
  {
    case 0u:
    case 2u:
    {
      if(viewZ >= kInvalidDenoiserViewZThreshold)
      {
        outputRadiance = rawBeauty.rgb;
      }
      break;
    }
    case 1u:
      outputRadiance = rawBeauty.rgb;
      break;
    case 3u:
      outputRadiance = noisyRemodulatedDiffuse;
      break;
    case 4u:
      outputRadiance = noisyRemodulatedSpecular;
      break;
    case 5u:
      outputRadiance = remodulatedDiffuse;
      break;
    case 6u:
      outputRadiance = remodulatedSpecular;
      break;
    case 7u:
      outputRadiance = VisualizeNormalRoughness(normalRoughness);
      break;
    case 8u:
      outputRadiance = VisualizeViewZ(viewZ);
      break;
    case 9u:
      outputRadiance = VisualizeMotion(motionVectors);
      break;
    default:
      outputRadiance = combinedRadiance;
      break;
  }

  outImage[int2(dispatchThreadID.xy)] = float4(outputRadiance, 1.0);
}
