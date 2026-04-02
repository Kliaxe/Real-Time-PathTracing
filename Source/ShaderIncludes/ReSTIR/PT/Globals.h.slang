#ifndef RESTIR_PT_GLOBALS_H
#define RESTIR_PT_GLOBALS_H

#include <nvshaders/slang_types.h>
#include "ShaderIo.h"

[[vk::push_constant]] ConstantBuffer<ReSTIRPTPushConstant> pushConst;
[[vk::binding(ReSTIRBindingPoints::eReSTIRTextures)]] Sampler2D textures[];
[[vk::binding(ReSTIRBindingPoints::eReSTIRTlas)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(ReSTIRBindingPoints::eReSTIROutputImage)]] RWTexture2D<float4> outImage;
[[vk::binding(ReSTIRBindingPoints::eReSTIRAccumulationImage)]] RWTexture2D<float4> accumulationImage;
[[vk::binding(ReSTIRBindingPoints::eReSTIRInitialReservoirBuffer)]] RWStructuredBuffer<ReSTIRPTReservoir> initialReservoirBuffer;
[[vk::binding(ReSTIRBindingPoints::eReSTIRScratchReservoirBuffer)]] RWStructuredBuffer<ReSTIRPTReservoir> scratchReservoirBuffer;
[[vk::binding(ReSTIRBindingPoints::eReSTIRCurrentHistoryReservoirBuffer)]] RWStructuredBuffer<ReSTIRPTReservoir> currentHistoryReservoirBuffer;
[[vk::binding(ReSTIRBindingPoints::eReSTIRPreviousHistoryReservoirBuffer)]] StructuredBuffer<ReSTIRPTReservoir> previousHistoryReservoirBuffer;
[[vk::binding(ReSTIRBindingPoints::eReSTIRCurrentSurfaceBuffer)]] RWStructuredBuffer<ReSTIRPTPrimarySurface> currentSurfaceBuffer;
[[vk::binding(ReSTIRBindingPoints::eReSTIRPreviousSurfaceBuffer)]] StructuredBuffer<ReSTIRPTPrimarySurface> previousSurfaceBuffer;
[[vk::binding(ReSTIRBindingPoints::eReSTIRNeighborOffsetBuffer)]] StructuredBuffer<ReSTIRNeighborOffset> neighborOffsetBuffer;
[[vk::binding(ReSTIRBindingPoints::eReSTIRDebugBuffer)]] RWStructuredBuffer<ReSTIRDebugPixel> debugBuffer;
[[vk::binding(ReSTIRBindingPoints::eReSTIRMotionVectorsImage)]] RWTexture2D<float4> motionVectorsImage;
[[vk::binding(ReSTIRBindingPoints::eReSTIRNormalRoughnessImage)]] RWTexture2D<float4> normalRoughnessImage;
[[vk::binding(ReSTIRBindingPoints::eReSTIRBaseColorMetalnessImage)]] RWTexture2D<float4> baseColorMetalnessImage;
[[vk::binding(ReSTIRBindingPoints::eReSTIRViewZImage)]] RWTexture2D<float> viewZImage;
[[vk::binding(ReSTIRBindingPoints::eReSTIRDiffuseRadianceHitDistanceImage)]] RWTexture2D<float4> diffuseRadianceHitDistanceImage;
[[vk::binding(ReSTIRBindingPoints::eReSTIRSpecularRadianceHitDistanceImage)]] RWTexture2D<float4> specularRadianceHitDistanceImage;

#endif
