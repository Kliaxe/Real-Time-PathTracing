#ifndef RESTIR_PARAMETERS_H
#define RESTIR_PARAMETERS_H

#ifdef __cplusplus
#include <stdint.h>
#else
#define uint32_t uint
#define RESTIR_DEFAULT(value) = value
#endif // __cplusplus

// Reservoirs are stored in a structured buffer in a block-linear layout.
// This constant defines the size of that block, measured in pixels.
#define RESTIR_RESERVOIR_BLOCK_SIZE 16

// Bias correction modes for temporal and spatial resampling:
// Use (1/M) normalization, which is very biased but also very fast.
#define RESTIR_BIAS_CORRECTION_OFF 0
// Use MIS-like normalization but assume that every sample is visible.
#define RESTIR_BIAS_CORRECTION_BASIC 1
// Use MIS-like normalization with visibility rays. This is the strongest bias-reduction mode in this implementation.
#define RESTIR_BIAS_CORRECTION_RAY_TRACED 2

// When neighboring samples have less than the naive sampling M threshold, they are ignored during spatial resampling.
#define RESTIR_NAIVE_SAMPLING_M_THRESHOLD 2

#define RESTIR_INVALID_LIGHT_INDEX (0xffffffffu)

#ifndef __cplusplus
static const uint ReSTIRInvalidLightIndex = RESTIR_INVALID_LIGHT_INDEX;
#endif

struct ReSTIRLightBufferRegion
{
    uint32_t firstLightIndex;
    uint32_t numLights;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIREnvironmentLightBufferParameters
{
    uint32_t lightPresent;
    uint32_t lightIndex;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIRRuntimeParameters
{
    uint32_t neighborOffsetMask; // Spatial
    uint32_t frameIndex;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIRLightBufferParameters
{
    ReSTIRLightBufferRegion localLightBufferRegion;
    ReSTIREnvironmentLightBufferParameters environmentLightParams;
};

struct ReSTIRReservoirBufferParameters
{
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIRPackedDIReservoir
{
    uint32_t lightData;
    uint32_t uvData;
    uint32_t mVisibility;
    uint32_t distanceAge;
    float targetPdf;
    float weight;
};

#ifdef __cplusplus
enum class ReSTIRDI_TemporalBiasCorrectionMode : uint32_t
{
    Off = RESTIR_BIAS_CORRECTION_OFF,
    Basic = RESTIR_BIAS_CORRECTION_BASIC,
    Raytraced = RESTIR_BIAS_CORRECTION_RAY_TRACED
};

enum class ReSTIRDI_SpatialBiasCorrectionMode : uint32_t
{
    Off = RESTIR_BIAS_CORRECTION_OFF,
    Basic = RESTIR_BIAS_CORRECTION_BASIC,
    Raytraced = RESTIR_BIAS_CORRECTION_RAY_TRACED
};
#else
#define ReSTIRDI_TemporalBiasCorrectionMode uint32_t
#define ReSTIRDI_SpatialBiasCorrectionMode uint32_t
#endif

struct ReSTIRDIBufferIndices
{
    uint32_t initialSamplingOutputBufferIndex;
    uint32_t temporalResamplingInputBufferIndex;
    uint32_t temporalResamplingOutputBufferIndex;
    uint32_t spatialResamplingInputBufferIndex;

    uint32_t spatialResamplingOutputBufferIndex;
    uint32_t shadingInputBufferIndex;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIRDIInitialSamplingParameters
{
    uint32_t numLocalLightSamples;
    uint32_t numEnvironmentSamples;
    uint32_t numBrdfSamples;
    uint32_t pad0;

    float brdfCutoff;
    float brdfRayMinT;
    uint32_t enableInitialVisibility;

    uint32_t environmentMapImportanceSampling; // Only used in InitialSampling.hlsli via DIEvaluateEnvironmentMapSamplingPdf
    uint32_t pad1;
    uint32_t pad2;
    uint32_t pad3;
    uint32_t pad4;
};

struct ReSTIRDITemporalResamplingParameters
{
    // Maximum history length for temporal reuse, measured in frames.
    // Higher values result in more stable and high quality sampling, at the cost of slow reaction to changes.
    uint32_t maxHistoryLength;

    // Controls the bias correction math for temporal reuse. Depending on the setting, it can add
    // some shader cost and one approximate shadow ray per pixel.
    // Ideally, these rays should be traced through the previous frame's BVH for a stricter temporal visibility test.
    ReSTIRDI_TemporalBiasCorrectionMode biasCorrectionMode;

    // Surface depth similarity threshold for temporal reuse.
    // If the previous frame surface's depth is within this threshold from the current frame surface's depth,
    // the surfaces are considered similar. The threshold is relative, i.e. 0.1 means 10% of the current depth.
    // Otherwise, the pixel is not reused, and the resampling shader will look for a different one.
    float depthThreshold;

    // Surface normal similarity threshold for temporal reuse.
    // If the dot product of two surfaces' normals is higher than this threshold, the surfaces are considered similar.
    // Otherwise, the pixel is not reused, and the resampling shader will look for a different one.
    float normalThreshold;

    // Allows the temporal resampling logic to skip the bias correction ray trace for light samples
    // reused from the previous frame. Only safe to use when invisible light samples are discarded
    // on the previous frame, then any sample coming from the previous frame can be assumed visible.
    uint32_t enableVisibilityShortcut;

    // Enables permuting the pixels sampled from the previous frame in order to decorrelate temporal reuse.
    uint32_t enablePermutationSampling;

    // Random number for permutation sampling that is the same for all pixels in the frame.
    uint32_t uniformRandomNumber;

    uint32_t pad1;
};

struct ReSTIRDISpatialResamplingParameters
{
    // Number of neighbor pixels considered for resampling (1-32).
    // Some of them may be skipped if they fail the surface similarity test.
    uint32_t numSamples;

    // Number of neighbor pixels considered when there is not enough history data (1-32).
    // Setting this parameter equal or lower than `numSamples` effectively disables the disocclusion boost.
    uint32_t numDisocclusionBoostSamples;

    // Screen-space radius for spatial resampling, measured in pixels.
    float samplingRadius;

    // Controls the bias correction math for spatial reuse. Depending on the setting, it can add
    // some shader cost and one approximate shadow ray per spatial sample.
    ReSTIRDI_SpatialBiasCorrectionMode biasCorrectionMode;

    // Surface depth similarity threshold for spatial reuse.
    // See ReSTIRDITemporalResamplingParameters::depthThreshold for more information.
    float depthThreshold;

    // Surface normal similarity threshold for spatial reuse.
    // See ReSTIRDITemporalResamplingParameters::normalThreshold for more information.
    float normalThreshold;

    // Disocclusion boost is activated when the current reservoir's M value is less than targetHistoryLength.
    uint32_t targetHistoryLength;

    // Enables the comparison of surface materials before taking a surface into resampling.
    uint32_t enableMaterialSimilarityTest;

    // Prevents samples from the current frame, or with too little temporal history, from being spread to neighbors.
    uint32_t discountNaiveSamples;

    uint32_t pad1;
    uint32_t pad2;
    uint32_t pad3;
};

struct ReSTIRDIShadingParameters
{
    uint32_t enableFinalVisibility;
    uint32_t reuseFinalVisibility;
    uint32_t finalVisibilityMaxAge;
    float finalVisibilityMaxDistance;
};

#endif // RESTIR_PARAMETERS_H
