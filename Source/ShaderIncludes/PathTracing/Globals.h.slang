#ifndef PATH_TRACING_GLOBALS_H
#define PATH_TRACING_GLOBALS_H

#include <nvshaders/slang_types.h>
#include "ShaderIo.h"

// Shared path tracing resources owned by the ray tracing pipeline translation unit.
[[vk::push_constant]]                              ConstantBuffer<PathTracePushConstant> pushConst;
[[vk::binding(BindingPoints::eTextures)]]          Sampler2D textures[];
[[vk::binding(BindingPoints::eTlas)]]              RaytracingAccelerationStructure topLevelAS;
[[vk::binding(BindingPoints::eOutputImage)]]       RWTexture2D<float4> outImage;
[[vk::binding(BindingPoints::eAccumulationImage)]] RWTexture2D<float4> accumulationImage;
[[vk::binding(BindingPoints::eMotionVectorsImage)]] RWTexture2D<float4> motionVectorsImage;
[[vk::binding(BindingPoints::eNormalRoughnessImage)]] RWTexture2D<float4> normalRoughnessImage;
[[vk::binding(BindingPoints::eBaseColorMetalnessImage)]] RWTexture2D<float4> baseColorMetalnessImage;
[[vk::binding(BindingPoints::eViewZImage)]]       RWTexture2D<float> viewZImage;
[[vk::binding(BindingPoints::eDiffuseRadianceHitDistanceImage)]] RWTexture2D<float4> diffuseRadianceHitDistanceImage;
[[vk::binding(BindingPoints::eSpecularRadianceHitDistanceImage)]] RWTexture2D<float4> specularRadianceHitDistanceImage;

#endif


