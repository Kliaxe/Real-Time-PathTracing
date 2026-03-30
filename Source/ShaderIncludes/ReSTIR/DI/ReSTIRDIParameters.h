/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef RESTIR_DI_PARAMETERS_H
#define RESTIR_DI_PARAMETERS_H

#include "ReSTIR/Common/ReSTIRParameters.h"
#include "ReSTIR/Common/ReSTIRTypes.h"

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
    Pairwise = RESTIR_BIAS_CORRECTION_PAIRWISE,
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
    uint32_t numInfiniteLightSamples;
    uint32_t numEnvironmentSamples;
    uint32_t numBrdfSamples;

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
    // some shader cost and one approximate shadow ray per pixel (or per two pixels if checkerboard sampling is enabled).
    // Ideally, these rays should be traced through the previous frame's BVH to get fully unbiased results.
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

    // Enables permuting the pixels sampled from the previous frame in order to add temporal
    // variation to the output signal and make it more denoiser friendly.
    uint32_t enablePermutationSampling;

    // Random number for permutation sampling that is the same for all pixels in the frame
    uint32_t uniformRandomNumber;

    float permutationSamplingThreshold; // Not used in TemporalResampling.hlsli
};

struct ReSTIRDISpatialResamplingParameters
{
    // Number of neighbor pixels considered for resampling (1-32)
    // Some of the may be skipped if they fail the surface similarity test.
    uint32_t numSamples;

    // Number of neighbor pixels considered when there is not enough history data (1-32)
    // Setting this parameter equal or lower than `numSpatialSamples` effectively
    // disables the disocclusion boost.
    uint32_t numDisocclusionBoostSamples;

    // Screen-space radius for spatial resampling, measured in pixels.
    float samplingRadius;

    // Controls the bias correction math for spatial reuse. Depending on the setting, it can add
    // some shader cost and one approximate shadow ray *per every spatial sample* per pixel
    // (or per two pixels if checkerboard sampling is enabled).
    ReSTIRDI_SpatialBiasCorrectionMode biasCorrectionMode;

    // Surface depth similarity threshold for spatial reuse.
    // See 'ReSTIRDITemporalResamplingParameters::depthThreshold' for more information.
    float depthThreshold;

    // Surface normal similarity threshold for spatial reuse.
    // See 'ReSTIRDITemporalResamplingParameters::normalThreshold' for more information.
    float normalThreshold;

    // Disocclusion boost is activated when the current reservoir's M value
    // is less than targetHistoryLength.
    uint32_t targetHistoryLength;

    // Enables the comparison of surface materials before taking a surface into resampling.
    uint32_t enableMaterialSimilarityTest;

    // Prevents samples which are from the current frame or have no reasonable temporal history merged being spread to neighbors
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

    uint32_t enableDenoiserInputPacking;
    uint32_t pad1;
    uint32_t pad2;
    uint32_t pad3;
};

#endif // RESTIR_DI_PARAMETERS_H
