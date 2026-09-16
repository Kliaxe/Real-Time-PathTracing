#ifndef SHADERIO_H
#define SHADERIO_H

#include "Common/IoGltf.h"
#include "ReSTIR/PTParameters.h"

NAMESPACE_SHADERIO_BEGIN()

// BindingPoints
// Descriptor binding numbers are part of the CPU/HLSL ABI. Keep these values synchronized with descriptor-set creation in C++ and resource declarations in the corresponding shaders.

enum BindingPoints
{
  eTextures                         = 0,
  eTlas                             = 1,
  eOutputImage                      = 2,
  eAccumulationImage                = 3,
  eMotionVectorsImage               = 4,
  eNormalRoughnessImage             = 5,
  eBaseColorMetalnessImage          = 6,
  eViewZImage                       = 7,
  eDiffuseRadianceHitDistanceImage  = 8,
  eSpecularRadianceHitDistanceImage = 9,
  eSpecularDemodulationFactorImage  = 10,
  eBlueNoiseTexture                 = 11,
  // DXC represents descriptor-indexed textures and samplers as separate arrays.
  // These bindings are temporary companions to eTextures until every shader has been replaced, after which the legacy combined binding is deleted.
  eHlslTextures                     = 24,
  eHlslTextureSamplers              = 25,
};

// BlueNoiseDimensions
// Size of the spatiotemporal blue-noise texture array, shared so the CPU generator and the shaders that sample it agree.

enum BlueNoiseDimensions
{
  eBlueNoiseWidth  = 64,
  eBlueNoiseHeight = 64,
  eBlueNoiseLayers = 32,
};

// RasterPushConstant
// Per-draw push constants for the raster preview in Rasterizer.hlsl, filled by RasterRenderer.

struct RasterPushConstant
{
  // Inverse-transpose of the instance's upper 3x3 transform, for normals.
  float3x3       normalMatrix;

  // Instance to draw, or -1 for the fullscreen background triangle.
  int            instanceIndex;

  // Device address of the scene info buffer.
  RTPT_BUFFER_POINTER(GltfSceneInfo) sceneInfoAddress;
};

// PathTraceFlags
// Bits for PathTracePushConstant::flags.

enum PathTraceFlags
{
  ePathTraceFlagAccumulate           = 0x1u,
  ePathTraceFlagWriteDenoiserSignals = 0x2u,
};

// PathTracePushConstant
// Per-frame push constants for the reference path tracer's ray tracing dispatch.

struct PathTracePushConstant
{
  // Device address of the scene info buffer.
  RTPT_BUFFER_POINTER(GltfSceneInfo) sceneInfoAddress;

  // Seeds the per-frame RNG; advances every frame.
  uint           rngFrameNumber;

  // Frames already in the accumulation image; 0 when accumulation is off.
  uint           accumulatedFrames;

  // Bounce limit, already clamped to the renderer's fixed maximum.
  uint           maxBounces;

  // PathTraceFlags bits.
  uint           flags;

  // REBLUR hit distance normalization (A, B, C). Supplied by the renderer from DenoiserSettings so the shader and nrd::ReblurSettings cannot disagree.
  float3         reblurHitDistanceParams;

  // Luminance cap on demodulated radiance before it is written for NRD; zero disables it. See ComputeDenoiserRadianceClamp.
  float          denoiserRadianceClamp;
};

// ReSTIRPTParameters
// Packed uniform block consumed by every ReSTIR PT pass. The nested structs come from ReSTIR/PTParameters.h, so C++ only fills values and keeps the layout stable.

struct ReSTIRPTParameters
{
  // ReSTIRPTRuntimeParameters block.
  ReSTIRPTRuntimeParameters            runtimeParams;

  // ReSTIRPTReservoirBufferParameters block.
  ReSTIRPTReservoirBufferParameters    reservoirBufferParams;

  // Which rotating reservoir array each pass reads and writes.
  ReSTIRPTBufferIndices                bufferIndices;

  // ReSTIRPTInitialSamplingParameters block.
  ReSTIRPTInitialSamplingParameters    initialSampling;

  // Shift mapping mode and reconnection criteria.
  ReSTIRPTShiftParameters              shift;

  // ReSTIRPTTemporalResamplingParameters block.
  ReSTIRPTTemporalResamplingParameters temporalResampling;

  // ReSTIRPTSpatialResamplingParameters block.
  ReSTIRPTSpatialResamplingParameters  spatialResampling;

  // Section 3 pairing texture descriptors, one per spatial neighbour.
  ReSTIRPTPairingTextureParameters     pairingTextures[RESTIR_PT_MAX_PAIRING_TEXTURES];

  // ReSTIRPTDecorrelationParameters block.
  ReSTIRPTDecorrelationParameters      decorrelation;

  // ReSTIRPTShadingParameters block.
  ReSTIRPTShadingParameters            shading;

  // ReSTIRPTNeeParameters block.
  ReSTIRPTNeeParameters                nee;
};

// ReSTIRPTBindingPoints
// Descriptor binding numbers for the ReSTIR PT passes. Part of the CPU/HLSL ABI: these values must match descriptor-set creation in ReSTIRPTRenderer and the register attributes in PTGlobals.hlsli.

enum ReSTIRPTBindingPoints
{
  eReSTIRPTTextures              = 0,
  eReSTIRPTTlas                  = 1,
  eReSTIRPTOutputImage           = 2,
  eReSTIRPTAccumulationImage     = 3,
  eReSTIRPTPathReservoirBuffer   = 4,
  eReSTIRPTCurrentSurfaceBuffer  = 5,
  eReSTIRPTPreviousSurfaceBuffer = 6,
  eReSTIRPTParamsBuffer          = 7,
  // Section 5 duplication map: one score per pixel, written at the end of a frame and read by the next frame's temporal pass.
  eReSTIRPTDuplicationBuffer     = 9,
  // Section 3 pairing textures, concatenated into one buffer, one packed delta per texel.
  eReSTIRPTPairingBuffer         = 10,
  // Section 3 shared shift results: one record per pixel per paired neighbour, written by the spatial pre-pass and read by both partners.
  eReSTIRPTPairedShiftBuffer     = 11,
  // Section 6.3 vector-valued resampling weights, one RGB value per pixel.
  eReSTIRPTShadingWeightBuffer   = 12,
  // Section 6.4 dual motion vectors: each frame's pixel-space motion, read back the next frame as the occluder's motion.
  eReSTIRPTMotionVectorBuffer    = 13,
  // Section 6.1 presampled light tiles, rebuilt each frame.
  eReSTIRPTLightTileBuffer       = 14,
  // Section 6.2.2 work list: one entry per (pixel, pairing slot) needing a shift.
  eReSTIRPTPrepassWorkBuffer     = 15,
  // Append count, then the indirect trace dimensions.
  eReSTIRPTPrepassCounterBuffer  = 16,
  // Per-pixel denoiser guides produced by initial sampling and consumed by final shading: first-bounce hit distances in x and y, and the specular share of the sampled path's energy in z.
  // None of it can be recovered from the reservoir: resampling replaces the stored path with a neighbour's, and the shift replays that path's prefix, so what happened to the segment leaving THIS pixel is only ever known to the pass that traced it.
  eReSTIRPTDenoiserGuideBuffer = 8,
  // NRD inputs. Guide buffers first, then the two demodulated radiance signals and the specular demodulation factor the compose pass needs to remodulate.
  eReSTIRPTMotionVectorsImage               = 17,
  eReSTIRPTNormalRoughnessImage             = 18,
  eReSTIRPTBaseColorMetalnessImage          = 19,
  eReSTIRPTViewZImage                       = 20,
  eReSTIRPTDiffuseRadianceHitDistanceImage  = 21,
  eReSTIRPTSpecularRadianceHitDistanceImage = 22,
  eReSTIRPTSpecularDemodulationFactorImage  = 23,
  eReSTIRPTHlslTextures                      = 24,
  eReSTIRPTHlslTextureSamplers               = 25,
  eReSTIRPTBlueNoiseTexture                   = 26,
};

// ReSTIRPTLightTileSample
// One presampled light. The inverse source PDF is stored rather than recomputed because recovering it costs the same CDF probe the tile exists to avoid.

struct ReSTIRPTLightTileSample
{
  // Index of the presampled light.
  uint  lightIndex;

  // Inverse of the PDF the light was drawn with.
  float invSourcePdf;
};

// ReSTIRPTFlags
// Bits for ReSTIRPTPushConstant::flags.

enum ReSTIRPTFlags
{
  // Accumulate the resolved image over frames (mirrors ePathTraceFlagAccumulate).
  eReSTIRPTFlagAccumulate = 0x1u,
  // Output the plain path-traced radiance for the same sample instead of the resampled estimate.
  // This is the correctness gate: with reuse disabled the two must converge to the same image, so the toggle makes the comparison direct.
  eReSTIRPTFlagReferenceRadiance = 0x2u,
  // Write the NRD guide buffers and split radiance signals from final shading.
  // Off unless NRD will actually consume them: the writes are seven storage-image stores per pixel and buy nothing in Off or Accumulate mode.
  eReSTIRPTFlagWriteDenoiserSignals = 0x4u,
  // Section 6.2.2. Dispatch the spatial pre-pass over a sorted work list instead of one invocation per pixel.
  // Uniform across the dispatch, so the branch on it in the pre-pass costs nothing.
  eReSTIRPTFlagSortedPrepass = 0x10u,
};

// ReSTIRPTPushConstant
// Per-frame push constants shared by the ReSTIR PT passes.

struct ReSTIRPTPushConstant
{
  // Device address of the scene info buffer.
  RTPT_BUFFER_POINTER(GltfSceneInfo) sceneInfoAddress;

  // Seeds the per-frame RNG. Kept separate from accumulatedFrames so the sampling sequence advances even when accumulation is reset.
  uint           rngFrameNumber;

  // Frames already in the accumulation image.
  uint           accumulatedFrames;

  // Bounce limit for traced paths.
  uint           maxBounces;

  // ReSTIRPTFlags bits.
  uint           flags;

  // REBLUR hit distance normalization (A, B, C). Supplied by the renderer from DenoiserSettings so the shader and nrd::ReblurSettings cannot disagree.
  float3         reblurHitDistanceParams;

  // Luminance cap on demodulated radiance before it is written for NRD; zero disables it. See ComputeDenoiserRadianceClamp.
  float          denoiserRadianceClamp;
};

// ReSTIRPTShiftOutcome
// Why a shift produced (or failed to produce) a sample. Ordered so that anything below eReSTIRPTShiftOutcomeSuccess is a rejection with a specific cause, which is what makes a failure histogram actionable.

enum ReSTIRPTShiftOutcome
{
  eReSTIRPTShiftOutcomeNoSource          = 0u,  // No valid reservoir to shift.
  eReSTIRPTShiftOutcomeNoReconnection    = 1u,  // Path carries no reconnection anchor.
  eReSTIRPTShiftOutcomeReplaySampleFail  = 2u,  // BSDF sampling failed during replay.
  eReSTIRPTShiftOutcomeReplayEscaped     = 3u,  // Replayed path missed where the base path hit.
  eReSTIRPTShiftOutcomeDegenerate        = 4u,  // Reconnection distance collapsed.
  eReSTIRPTShiftOutcomeOccluded          = 5u,  // Connection blocked or landed on other geometry.
  eReSTIRPTShiftOutcomePrevLobeUnsupported = 6u,  // Preserved lobe has no support at the predecessor.
  eReSTIRPTShiftOutcomeRcLobeUnsupported = 7u,  // Preserved lobe has no support at the reconnection vertex.
  eReSTIRPTShiftOutcomeBadDenominator    = 8u,  // Jacobian denominator non-positive or non-finite.
  eReSTIRPTShiftOutcomeReconnectionDisagreement = 9u,  // The offset path would not have chosen this reconnection vertex, so the shift is not invertible.
  // Temporal-specific rejections, recorded before a shift is even attempted.
  // Kept distinct so a low reuse rate can be attributed: reprojecting off-screen and failing the surface test call for completely different fixes.
  eReSTIRPTShiftOutcomeNoHistory          = 10u,  // Frame zero, or no surface at this pixel.
  eReSTIRPTShiftOutcomeReprojectionFailed = 11u,  // Behind the previous camera or outside its frustum.
  eReSTIRPTShiftOutcomeHistoryOutOfBounds = 12u,  // Reprojected outside the viewport.
  eReSTIRPTShiftOutcomeSurfaceMismatch    = 13u,  // History exists but describes different geometry.
  eReSTIRPTShiftOutcomeHistoryEmpty       = 14u,  // Reprojected onto a pixel holding no valid sample.
  eReSTIRPTShiftOutcomeSuccess            = 15u,
};

// ReSTIRPTSurface
// Per-pixel surface record written by initial sampling and read by later passes, so they never have to re-trace the primary ray or re-resolve the material.
// The shading terms are deliberately reduced: reuse replays and reconnects paths rather than re-evaluating shading, so only the terms that drive the shift's surface-similarity and reconnection tests are kept.
// A shift that needs the full material record rebuilds it from the hit identity stored here, which costs the same scene reads a closest hit would do and no ray at all.

struct ReSTIRPTSurface
{
  // World-space position of the primary hit.
  float3 worldPosition;

  // Linear depth of the primary hit.
  float  linearDepth;

  // Shading normal at the primary hit.
  float3 shadingNormal;

  // Material roughness at the primary hit.
  float  roughness;

  // Geometric (face) normal at the primary hit.
  float3 geometricNormal;

  // Material metalness at the primary hit.
  float  metallic;

  // Material albedo at the primary hit.
  float3 albedo;

  // 1 when the primary ray hit a surface; misses write an all-zero record.
  uint   valid;

  // Scene instance of the primary hit, and the triangle within its mesh.
  uint   instanceIndex;
  uint   primitiveIndex;

  // DXR barycentrics of the primary hit: the weights of the triangle's second and third vertices.
  // With the two indices above they name the hit exactly, which is what lets a later pass rebuild the full material record through LoadSurfaceDataFromHit instead of tracing a ray back at the point.
  float2 barycentrics;
};

// ReSTIRPTPairedShift
// Section 3. One shift of a pixel's own path into its paired partner's domain.
// Stored rather than recomputed because pairing is reciprocal: the shift A needs from B is the shift B computes into A. Each record is read twice: by its owner as an inverse shift, by its partner as a forward shift.

struct ReSTIRPTPairedShift
{
  // Shifted integrand F in the destination domain. Kept as full float RGB, not packed: it becomes the reused reservoir's F and its luminance is the target function, so precision loss here would bias resampling, not just dim a pixel.
  float3 integrand;

  // Equation 2's Jacobian for this shift.
  float  jacobian;

  // Equation 2's denominator evaluated in the DESTINATION domain. The partner needs it to rebase a reused path, which is why it cannot be recomputed from the Jacobian alone.
  float  destinationDenominator;

  // ReSTIRPTShiftOutcome, plus the validity flag in its high bit. A failed shift must be distinguishable from an absent one: the first is a null candidate whose confidence still counts, the second means the pair never formed.
  uint   outcome;
};

CHECK_STRUCT_ALIGNMENT(ReSTIRPTPairedShift)
CHECK_STRUCT_ALIGNMENT(ReSTIRPTParameters)
CHECK_STRUCT_ALIGNMENT(ReSTIRPTPushConstant)
CHECK_STRUCT_ALIGNMENT(ReSTIRPTSurface)
#ifdef __cplusplus
#include <cstddef>
#include <type_traits>

// Packed reservoir layout
// Supplemental Algorithm 1 compresses the ReSTIR PT reservoir from 88 to 64 bytes. Two reservoir sets are required for temporal reuse, so this size is the dominant term in the renderer's memory budget and must not drift silently.
// Total size alone is too weak a guard: a reordered, widened, or vector-typed field can preserve the size while moving every offset the shader reads from. Pinning the boundary offsets of each 16-byte row catches that at compile time.

static_assert(std::is_standard_layout<ReSTIRPTPackedReservoir>::value, "Packed ReSTIR PT reservoir must be standard layout for the offsets below to be meaningful.");
static_assert(sizeof(ReSTIRPTPackedReservoir) == 64, "Packed ReSTIR PT reservoir must stay 64 bytes to match the shader storage buffer.");
static_assert(alignof(ReSTIRPTPackedReservoir) == 4, "Packed ReSTIR PT reservoir must stay 4-byte aligned; a wider member would insert padding.");
static_assert(offsetof(ReSTIRPTPackedReservoir, ucw) == 0, "ReSTIR PT reservoir layout drifted.");
static_assert(offsetof(ReSTIRPTPackedReservoir, initRandomSeed) == 16, "ReSTIR PT reservoir layout drifted.");
static_assert(offsetof(ReSTIRPTPackedReservoir, rcVertexPrimitiveIndex) == 32, "ReSTIR PT reservoir layout drifted.");
static_assert(offsetof(ReSTIRPTPackedReservoir, rcVertexRadianceG) == 48, "ReSTIR PT reservoir layout drifted.");
static_assert(offsetof(ReSTIRPTPackedReservoir, sampleFrame) == 60, "ReSTIR PT reservoir layout drifted.");

// Parameter block layout
// Every parameter sub-block must end on a 16-byte boundary so the uniform block packs identically under C++ and DXC scalar-layout rules.

static_assert(sizeof(ReSTIRPTParameters) % 16 == 0, "ReSTIRPTParameters must stay 16-byte aligned for constant buffer packing.");
static_assert(offsetof(ReSTIRPTParameters, bufferIndices) == 32, "ReSTIRPTParameters layout drifted.");
static_assert(offsetof(ReSTIRPTParameters, shift) == 96, "ReSTIRPTParameters layout drifted.");
static_assert(offsetof(ReSTIRPTParameters, spatialResampling) == 144, "ReSTIRPTParameters layout drifted.");
// Three 16-byte pairing descriptors sit between spatialResampling and decorrelation, so both trailing blocks moved by 48 bytes.
static_assert(offsetof(ReSTIRPTParameters, pairingTextures) == 176, "ReSTIRPTParameters layout drifted.");
static_assert(offsetof(ReSTIRPTParameters, decorrelation) == 224, "ReSTIRPTParameters layout drifted.");
static_assert(offsetof(ReSTIRPTParameters, shading) == 240, "ReSTIRPTParameters layout drifted.");
static_assert(offsetof(ReSTIRPTParameters, nee) == 256, "ReSTIRPTParameters layout drifted.");
static_assert(sizeof(ReSTIRPTLightTileSample) == 8, "Light tile sample must match the shader storage buffer.");
#endif

NAMESPACE_SHADERIO_END()

#endif  // SHADERIO_H
