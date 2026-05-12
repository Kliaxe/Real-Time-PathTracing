#ifndef RESTIR_DI_GLOBALS_H
#define RESTIR_DI_GLOBALS_H

#define PATH_TRACING_CUSTOM_GLOBALS 1

#include <nvshaders/slang_types.h>
#include "ShaderIo.h"

[[vk::push_constant]] ConstantBuffer<ReSTIRDIPushConstant> pushConst;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDITextures)]] Sampler2D textures[];
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDITlas)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDIOuputImage)]] RWTexture2D<float4> outImage;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDIAccumulationImage)]] RWTexture2D<float4> accumulationImage;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDILightReservoirBuffer)]] RWStructuredBuffer<ReSTIRPackedDIReservoir> diReservoirBuffer;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDICurrentSurfaceBuffer)]] RWStructuredBuffer<ReSTIRDISurface> currentSurfaceBuffer;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDIPreviousSurfaceBuffer)]] StructuredBuffer<ReSTIRDISurface> previousSurfaceBuffer;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDINeighborOffsetBuffer)]] StructuredBuffer<float2> diNeighborOffsetBuffer;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDIParamsBuffer)]] ConstantBuffer<ReSTIRDIParameters> diParams;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDIDebugBuffer)]] RWStructuredBuffer<ReSTIRDebugPixel> debugBuffer;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDIMotionVectorsImage)]] RWTexture2D<float4> motionVectorsImage;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDINormalRoughnessImage)]] RWTexture2D<float4> normalRoughnessImage;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDIBaseColorMetalnessImage)]] RWTexture2D<float4> baseColorMetalnessImage;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDIViewZImage)]] RWTexture2D<float> viewZImage;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDIDiffuseRadianceHitDistanceImage)]] RWTexture2D<float4> diffuseRadianceHitDistanceImage;
[[vk::binding(ReSTIRDIBindingPoints::eReSTIRDISpecularRadianceHitDistanceImage)]] RWTexture2D<float4> specularRadianceHitDistanceImage;

#endif
