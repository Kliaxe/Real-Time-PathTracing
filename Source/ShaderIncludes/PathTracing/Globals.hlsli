#ifndef PATH_TRACING_GLOBALS_H
#define PATH_TRACING_GLOBALS_H

#include <Common/ShaderTypes.h>
#include "ShaderIo.h"

// Shared path tracing resources
// Owned by the reference ray tracing pipeline's translation unit. Common.hlsli includes this header unless PATH_TRACING_CUSTOM_GLOBALS is defined, which lets ReSTIR PT supply its own resources under the same names.
// Binding numbers come from BindingPoints in ShaderIo.h and must match descriptor-set creation on the CPU.

[[vk::push_constant]]                              ConstantBuffer<PathTracePushConstant> pushConst;
[[vk::binding(BindingPoints::eHlslTextures)]]      Texture2D<float4> textures[];
[[vk::binding(BindingPoints::eHlslTextureSamplers)]] SamplerState textureSamplers[];
[[vk::binding(BindingPoints::eTlas)]]              RaytracingAccelerationStructure topLevelAS;
[[vk::binding(BindingPoints::eOutputImage)]]       RWTexture2D<float4> outImage;
[[vk::binding(BindingPoints::eAccumulationImage)]] RWTexture2D<float4> accumulationImage;

// Denoiser inputs
// NRD guide buffers and the split noisy radiance signals, written by the ray generation shader when denoiser signals are requested.

[[vk::binding(BindingPoints::eMotionVectorsImage)]] RWTexture2D<float4> motionVectorsImage;
[[vk::binding(BindingPoints::eNormalRoughnessImage)]] RWTexture2D<float4> normalRoughnessImage;
[[vk::binding(BindingPoints::eBaseColorMetalnessImage)]] RWTexture2D<float4> baseColorMetalnessImage;
[[vk::binding(BindingPoints::eViewZImage)]]       RWTexture2D<float> viewZImage;
[[vk::binding(BindingPoints::eDiffuseRadianceHitDistanceImage)]] RWTexture2D<float4> diffuseRadianceHitDistanceImage;
[[vk::binding(BindingPoints::eSpecularRadianceHitDistanceImage)]] RWTexture2D<float4> specularRadianceHitDistanceImage;
[[vk::binding(BindingPoints::eSpecularDemodulationFactorImage)]] RWTexture2D<float4> specularDemodulationFactorImage;

// Sampling
// Shared rank volume read directly for each addressable path-sampling dimension.

[[vk::binding(BindingPoints::eBlueNoiseTexture)]] Texture2DArray<uint> blueNoiseTexture;

// Samples a scene texture at an explicit mip, since ray tracing stages have no derivatives for implicit LOD.
// DXC splits descriptor-indexed textures and samplers into separate arrays that share one index.
float4 SampleSceneTextureLevel(int textureIndex, float2 textureCoordinate, float mipLevel)
{
  const uint index = NonUniformResourceIndex(uint(textureIndex));

  return textures[index].SampleLevel(textureSamplers[index], textureCoordinate, mipLevel);
}

#endif
