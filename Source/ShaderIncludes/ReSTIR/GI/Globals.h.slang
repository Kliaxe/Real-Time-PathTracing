#ifndef RESTIR_GI_GLOBALS_H
#define RESTIR_GI_GLOBALS_H

#ifndef PATH_TRACING_CUSTOM_GLOBALS
#define PATH_TRACING_CUSTOM_GLOBALS 1
#endif

#include <nvshaders/slang_types.h>
#include "ShaderIo.h"

[[vk::push_constant]] ConstantBuffer<ReSTIRGIPushConstant> pushConst;
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGITextures)]] Sampler2D textures[];
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGITlas)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGIOutputImage)]] RWTexture2D<float4> outImage;
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGIAccumulationImage)]] RWTexture2D<float4> accumulationImage;
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGIReservoirBuffer)]] RWStructuredBuffer<ReSTIRGIReservoir> giReservoirBuffer;
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGIInitialSampleBuffer)]] RWStructuredBuffer<ReSTIRGIReservoir> giInitialSampleBuffer;
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGICurrentSurfaceBuffer)]] RWStructuredBuffer<ReSTIRGIPrimarySurface> currentSurfaceBuffer;
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGIPreviousSurfaceBuffer)]] StructuredBuffer<ReSTIRGIPrimarySurface> previousSurfaceBuffer;
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGINeighborOffsetBuffer)]] StructuredBuffer<float2> giNeighborOffsetBuffer;
[[vk::binding(ReSTIRGIBindingPoints::eReSTIRGIParamsBuffer)]] ConstantBuffer<ReSTIRGIParameters> giParams;

#endif
