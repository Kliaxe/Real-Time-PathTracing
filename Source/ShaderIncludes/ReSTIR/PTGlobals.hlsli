#ifndef RESTIR_PT_GLOBALS_H
#define RESTIR_PT_GLOBALS_H

// Tells the shared path tracing headers not to declare their own bindings; the ReSTIR PT descriptor set is declared below instead.
// The path tracing code is otherwise reused verbatim, which is what keeps initial sampling identical to the reference tracer.
#define PATH_TRACING_CUSTOM_GLOBALS 1

#include <Common/ShaderTypes.h>
#include "ShaderIo.h"

// Descriptor declarations
// Every ReSTIR PT shader sees this one set.
// The numeric binding points come from ShaderIo.h and must match the descriptor set built by ReSTIRPTRenderer on the CPU.

[[vk::push_constant]] ConstantBuffer<ReSTIRPTPushConstant> pushConst;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTHlslTextures)]] Texture2D<float4> textures[];
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTHlslTextureSamplers)]] SamplerState textureSamplers[];
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTTlas)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTOutputImage)]] RWTexture2D<float4> outImage;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTAccumulationImage)]] RWTexture2D<float4> accumulationImage;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTPathReservoirBuffer)]] RWStructuredBuffer<ReSTIRPTPackedReservoir> ptReservoirBuffer;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTCurrentSurfaceBuffer)]] RWStructuredBuffer<ReSTIRPTSurface> currentSurfaceBuffer;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTPreviousSurfaceBuffer)]] StructuredBuffer<ReSTIRPTSurface> previousSurfaceBuffer;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTParamsBuffer)]] ConstantBuffer<ReSTIRPTParameters> ptParams;
// Section 5. Indexed by pixel (row-major, not the block-linear reservoir layout): the map is consumed by a plain backprojection lookup, which has no reason to pay for swizzling.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTDuplicationBuffer)]] RWStructuredBuffer<float> ptDuplicationBuffer;
// Section 3. All pairing textures concatenated; each texel packs the two signed deltas biased by 128 into the low 16 bits.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTPairingBuffer)]] StructuredBuffer<uint> ptPairingBuffer;
// Section 3. Shared shift results, written by the spatial pre-pass and read by both members of each pair.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTPairedShiftBuffer)]] RWStructuredBuffer<ReSTIRPTPairedShift> ptPairedShiftBuffer;
// Section 6.3. Written by whichever pass produced the reservoir final shading reads, and consumed only by final shading. Separate from the reservoir because the reservoir is also next frame's temporal history: overwriting its F or ucw with a shading-only quantity would corrupt future resampling.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTShadingWeightBuffer)]] RWStructuredBuffer<float3> ptShadingWeightBuffer;
// Section 6.4. One pixel-space motion vector per pixel, from the PREVIOUS frame. A single buffer suffices because each invocation reads its own element before overwriting it, so there is no cross-thread ordering to establish.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTMotionVectorBuffer)]] RWStructuredBuffer<float2> ptMotionVectorBuffer;
// Section 6.1. RESTIR_PT_LIGHT_TILE_COUNT tiles of RESTIR_PT_LIGHT_TILE_SIZE presampled lights, laid out tile-major so one screen tile's candidates are contiguous.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTLightTileBuffer)]] RWStructuredBuffer<ReSTIRPTLightTileSample> ptLightTileBuffer;
// Section 6.2.2. The compacted (pixel, slot) work list the spatial pre-pass consumes.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTPrepassWorkBuffer)]] RWStructuredBuffer<uint> ptPrepassWorkBuffer;
// Section 6.2.2. The work list's append count followed by the indirect trace dimensions.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTPrepassCounterBuffer)]] RWStructuredBuffer<uint> ptPrepassCounterBuffer;
// NRD guides from initial sampling: x and y are the diffuse and specular first-bounce hit distances, where zero means that lobe was not the one sampled this frame; z is the specular share of the path's energy, or -1 when the path carried none and the share is undefined.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTDenoiserGuideBuffer)]] RWStructuredBuffer<float3> ptDenoiserGuideBuffer;

// Shared sampling data used by initial tracing and source-domain replay.
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTBlueNoiseTexture)]] Texture2DArray<uint> blueNoiseTexture;

// NRD inputs
// Always bound, written only when eReSTIRPTFlagWriteDenoiserSignals is set.
// A descriptor that is bound but unwritten costs nothing, whereas leaving the binding out would need a second descriptor layout for the denoised path.

[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTMotionVectorsImage)]] RWTexture2D<float4> motionVectorsImage;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTNormalRoughnessImage)]] RWTexture2D<float4> normalRoughnessImage;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTBaseColorMetalnessImage)]] RWTexture2D<float4> baseColorMetalnessImage;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTViewZImage)]] RWTexture2D<float> viewZImage;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTDiffuseRadianceHitDistanceImage)]] RWTexture2D<float4> diffuseRadianceHitDistanceImage;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTSpecularRadianceHitDistanceImage)]] RWTexture2D<float4> specularRadianceHitDistanceImage;
[[vk::binding(ReSTIRPTBindingPoints::eReSTIRPTSpecularDemodulationFactorImage)]] RWTexture2D<float4> specularDemodulationFactorImage;

// Binds the reservoir storage helpers to the buffer declared above.
#define RESTIR_PT_RESERVOIR_BUFFER ptReservoirBuffer

float4 SampleSceneTextureLevel(int textureIndex, float2 textureCoordinate, float mipLevel)
{
  const uint index = NonUniformResourceIndex(uint(textureIndex));

  return textures[index].SampleLevel(textureSamplers[index], textureCoordinate, mipLevel);
}

#endif

