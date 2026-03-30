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

#ifndef RESTIR_DI_RESERVOIR_HLSLI
#define RESTIR_DI_RESERVOIR_HLSLI

#include "ReSTIR/Common/ReSTIRParameters.h"

// This structure represents a single light reservoir that stores the weights, the sample ref,
// sample count (M), and visibility for reuse. It can be serialized into ReSTIRPackedDIReservoir for storage.
struct ReSTIRDIReservoir
{
    // Light index (bits 0..30) and validity bit (31)
    uint lightData;

    // Sample UV encoded in 16-bit fixed point format
    uint uvData;

    // Overloaded: represents RIS weight sum during streaming,
    // then reservoir weight (inverse PDF) after FinalizeResampling
    float weightSum;

    // Target PDF of the selected sample
    float targetPdf;

    // Number of samples considered for this reservoir (pairwise MIS makes this a float)
    float M;

    // Visibility information stored in the reservoir for reuse
    uint packedVisibility;

    // Screen-space distance between the current location of the reservoir
    // and the location where the visibility information was generated,
    // minus the motion vectors applied in temporal resampling
    int2 spatialDistance;

    // How many frames ago the visibility information was generated
    uint age;

    // Cannonical weight when using pairwise MIS (ignored except during pairwise MIS computations)
    float canonicalWeight;
};

// Encoding helper constants for ReSTIRPackedDIReservoir.mVisibility
static const uint ReSTIRPackedDIReservoirVisibilityMask = 0x3ffff;
static const uint ReSTIRPackedDIReservoirVisibilityChannelMax = 0x3f;
static const uint ReSTIRPackedDIReservoirVisibilityChannelShift = 6;
static const uint ReSTIRPackedDIReservoirMShift = 18;
static const uint ReSTIRPackedDIReservoirMaxM = 0x3fff;

// Light index helpers
static const uint ReSTIRDIReservoirLightValidBit = 0x80000000;
static const uint ReSTIRDIReservoirLightIndexMask = 0x7FFFFFFF;

ReSTIRDIReservoir EmptyDIReservoir()
{
    ReSTIRDIReservoir s;
    s.lightData = 0;
    s.uvData = 0;
    s.targetPdf = 0;
    s.weightSum = 0;
    s.M = 0;
    s.packedVisibility = 0;
    s.spatialDistance = int2(0, 0);
    s.age = 0;
    s.canonicalWeight = 0;
    return s;
}

void StoreVisibilityInDIReservoir(
    inout ReSTIRDIReservoir reservoir,
    float3 visibility,
    bool discardIfInvisible)
{
    reservoir.packedVisibility = uint(saturate(visibility.x) * ReSTIRPackedDIReservoirVisibilityChannelMax) 
        | (uint(saturate(visibility.y) * ReSTIRPackedDIReservoirVisibilityChannelMax)) << ReSTIRPackedDIReservoirVisibilityChannelShift
        | (uint(saturate(visibility.z) * ReSTIRPackedDIReservoirVisibilityChannelMax)) << (ReSTIRPackedDIReservoirVisibilityChannelShift * 2);

    reservoir.spatialDistance = int2(0, 0);
    reservoir.age = 0;

    if (discardIfInvisible && visibility.x == 0 && visibility.y == 0 && visibility.z == 0)
    {
        // Keep M for correct resampling, remove the actual sample
        reservoir.lightData = 0;
        reservoir.weightSum = 0;
    }
}

// Structure that groups the parameters for GetDIReservoirVisibility(...)
// Reusing final visibility reduces the number of high-quality shadow rays needed to shade
// the scene, at the cost of somewhat softer or laggier shadows.
struct ReSTIRDIVisibilityReuseParameters
{
    // Controls the maximum age of the final visibility term, measured in frames, that can be reused from the
    // previous frame(s). Higher values result in better performance.
    uint maxAge;

    // Controls the maximum distance in screen space between the current pixel and the pixel that has
    // produced the final visibility term. The distance does not include the motion vectors.
    // Higher values result in better performance and softer shadows.
    float maxDistance;
};

bool GetDIReservoirVisibility(
    const ReSTIRDIReservoir reservoir,
    const ReSTIRDIVisibilityReuseParameters params,
    out float3 o_visibility)
{
    if (reservoir.age > 0 &&
        reservoir.age <= params.maxAge &&
        length(float2(reservoir.spatialDistance)) < params.maxDistance)
    {
        o_visibility.x = float(reservoir.packedVisibility & ReSTIRPackedDIReservoirVisibilityChannelMax) / ReSTIRPackedDIReservoirVisibilityChannelMax;
        o_visibility.y = float((reservoir.packedVisibility >> ReSTIRPackedDIReservoirVisibilityChannelShift) & ReSTIRPackedDIReservoirVisibilityChannelMax) / ReSTIRPackedDIReservoirVisibilityChannelMax;
        o_visibility.z = float((reservoir.packedVisibility >> (ReSTIRPackedDIReservoirVisibilityChannelShift * 2)) & ReSTIRPackedDIReservoirVisibilityChannelMax) / ReSTIRPackedDIReservoirVisibilityChannelMax;

        return true;
    }

    o_visibility = float3(0, 0, 0);
    return false;
}

bool IsValidDIReservoir(const ReSTIRDIReservoir reservoir)
{
    return reservoir.lightData != 0;
}

uint GetDIReservoirLightIndex(const ReSTIRDIReservoir reservoir)
{
    return reservoir.lightData & ReSTIRDIReservoirLightIndexMask;
}

float2 GetDIReservoirSampleUv(const ReSTIRDIReservoir reservoir)
{
    return float2(reservoir.uvData & 0xffff, reservoir.uvData >> 16) / float(0xffff);
}

float GetDIReservoirInvPdf(const ReSTIRDIReservoir reservoir)
{
    return reservoir.weightSum;
}

// Adds a new, non-reservoir light sample into the reservoir, returns true if this sample was selected.
// Algorithm (3) from the ReSTIR paper, Streaming RIS using weighted reservoir sampling.
bool StreamDIReservoirSample(
    inout ReSTIRDIReservoir reservoir,
    uint lightIndex,
    float2 uv,
    float random,
    float targetPdf,
    float invSourcePdf)
{
    // What's the current weight
    float risWeight = targetPdf * invSourcePdf;

    // Add one sample to the counter
    reservoir.M += 1;

    // Update the weight sum
    reservoir.weightSum += risWeight;

    // Decide if we will randomly pick this sample
    bool selectSample = (random * reservoir.weightSum < risWeight);

    // If we did select this sample, update the relevant data.
    // New samples don't have visibility or age information, we can skip that.
    if (selectSample)
    {
        reservoir.lightData = lightIndex | ReSTIRDIReservoirLightValidBit;
        reservoir.uvData = uint(saturate(uv.x) * 0xffff) | (uint(saturate(uv.y) * 0xffff) << 16);
        reservoir.targetPdf = targetPdf;
    }

    return selectSample;
}

// Adds `newReservoir` into `reservoir`, returns true if the new reservoir's sample was selected.
// This is a very general form, allowing input parameters to specfiy normalization and targetPdf
// rather than computing them from `newReservoir`.  Named "internal" since these parameters take
// different meanings (e.g., in CombineDIReservoirs() or StreamNeighborWithPairwiseMIS())
bool InternalSimpleResample(
    inout ReSTIRDIReservoir reservoir,
    const ReSTIRDIReservoir newReservoir,
    float random,
    float targetPdf RESTIR_DEFAULT(1.0f),            // Usually closely related to the sample normalization, 
    float sampleNormalization RESTIR_DEFAULT(1.0f),  //     typically off by some multiplicative factor 
    float sampleM RESTIR_DEFAULT(1.0f)               // In its most basic form, should be newReservoir.M
)
{
    // What's the current weight (times any prior-step RIS normalization factor)
    float risWeight = targetPdf * sampleNormalization;

    // Our *effective* candidate pool is the sum of our candidates plus those of our neighbors
    reservoir.M += sampleM;

    // Update the weight sum
    reservoir.weightSum += risWeight;

    // Decide if we will randomly pick this sample
    bool selectSample = (random * reservoir.weightSum < risWeight);

    // If we did select this sample, update the relevant data
    if (selectSample)
    {
        reservoir.lightData = newReservoir.lightData;
        reservoir.uvData = newReservoir.uvData;
        reservoir.targetPdf = targetPdf;
        reservoir.packedVisibility = newReservoir.packedVisibility;
        reservoir.spatialDistance = newReservoir.spatialDistance;
        reservoir.age = newReservoir.age;
    }

    return selectSample;
}

// Adds `newReservoir` into `reservoir`, returns true if the new reservoir's sample was selected.
// Algorithm (4) from the ReSTIR paper, Combining the streams of multiple reservoirs.
// Normalization - Equation (6) - is postponed until all reservoirs are combined.
bool CombineDIReservoirs(
    inout ReSTIRDIReservoir reservoir,
    const ReSTIRDIReservoir newReservoir,
    float random,
    float targetPdf)
{
    return InternalSimpleResample(
        reservoir,
        newReservoir,
        random,
        targetPdf,
        newReservoir.weightSum * newReservoir.M,
        newReservoir.M
    );
}

// Performs normalization of the reservoir after streaming. Equation (6) from the ReSTIR paper.
void FinalizeDIResampling(
    inout ReSTIRDIReservoir reservoir,
    float normalizationNumerator,
    float normalizationDenominator)
{
    float denominator = reservoir.targetPdf * normalizationDenominator;

    reservoir.weightSum = (denominator == 0.0) ? 0.0 : (reservoir.weightSum * normalizationNumerator) / denominator;
}

#endif // RESTIR_DI_RESERVOIR_HLSLI
