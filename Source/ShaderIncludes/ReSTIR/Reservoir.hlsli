#ifndef RESTIR_DI_RESERVOIR_HLSLI
#define RESTIR_DI_RESERVOIR_HLSLI

#include "ReSTIR/Parameters.h"

// Reservoir math for biased ReSTIR DI. The code separates streaming candidates,
// combining reservoirs, and final normalization so initial, temporal, and
// spatial passes can share the same mechanics.

// This structure represents one light reservoir. It stores one selected sample
// plus enough statistics to say how many candidates that sample represents.
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

    // Effective number of candidates represented by this reservoir. This is
    // the ReSTIR paper's M, not a matrix or material value.
    float M;

    // Visibility information stored in the reservoir for reuse
    uint packedVisibility;

    // Screen-space distance between the current location of the reservoir
    // and the location where the visibility information was generated,
    // minus the motion vectors applied in temporal resampling
    int2 spatialDistance;

    // How many frames ago the visibility information was generated
    uint age;

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
    return s;
}

void StoreVisibilityInDIReservoir(
    inout ReSTIRDIReservoir reservoir,
    float3 visibility,
    bool discardIfInvisible)
{
    // Pack RGB visibility into 6 bits per channel so it travels with the reservoir.
    reservoir.packedVisibility = uint(saturate(visibility.x) * ReSTIRPackedDIReservoirVisibilityChannelMax) 
        | (uint(saturate(visibility.y) * ReSTIRPackedDIReservoirVisibilityChannelMax)) << ReSTIRPackedDIReservoirVisibilityChannelShift
        | (uint(saturate(visibility.z) * ReSTIRPackedDIReservoirVisibilityChannelMax)) << (ReSTIRPackedDIReservoirVisibilityChannelShift * 2);

    // Fresh visibility starts at the reservoir's current screen position.
    reservoir.spatialDistance = int2(0, 0);
    reservoir.age = 0;

    if (discardIfInvisible && visibility.x == 0 && visibility.y == 0 && visibility.z == 0)
    {
        // Keep M for correct resampling, but remove the actual selected sample.
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
        // Visibility is approximate, but useful enough to skip a new final shadow ray.
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
    // After finalization, weightSum is the inverse PDF used by the shading estimator.
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
    // RIS weight is target PDF divided by the PDF that produced this sample.
    float risWeight = targetPdf * invSourcePdf;

    // Count the candidate even if it does not become the selected sample.
    reservoir.M += 1;

    // The streaming weight sum is needed for final reservoir normalization.
    reservoir.weightSum += risWeight;

    // Weighted reservoir sampling chooses the sample with probability proportional to its weight.
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
// This helper keeps target-pdf evaluation separate from reservoir combination so temporal and
// spatial reuse can evaluate the same candidate at the receiver where it will be reused.
bool InternalSimpleResample(
    inout ReSTIRDIReservoir reservoir,
    const ReSTIRDIReservoir newReservoir,
    float random,
    float targetPdf RESTIR_DEFAULT(1.0f),            // Target PDF of the reused sample at the current receiver.
    float sampleNormalization RESTIR_DEFAULT(1.0f),  // Weight carried from the source reservoir or pass.
    float sampleM RESTIR_DEFAULT(1.0f)               // Number of candidates represented by the source reservoir.
)
{
    // Reused reservoirs arrive with a normalization factor from their source pass.
    float risWeight = targetPdf * sampleNormalization;

    // The effective candidate count grows by the represented sample count of the reused reservoir.
    reservoir.M += sampleM;

    // Keep accumulating stream weights until the caller runs final normalization.
    reservoir.weightSum += risWeight;

    // The chosen representative can come from the center, temporal history, or a spatial neighbor.
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
    // If the selected sample has zero target density at this receiver, it cannot contribute.
    float denominator = reservoir.targetPdf * normalizationDenominator;

    // After finalization, weightSum stores the reservoir inverse PDF used in shading.
    reservoir.weightSum = (denominator == 0.0) ? 0.0 : (reservoir.weightSum * normalizationNumerator) / denominator;
}

#endif // RESTIR_DI_RESERVOIR_HLSLI
