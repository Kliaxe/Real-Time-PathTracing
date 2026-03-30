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

#ifndef RESTIR_DI_PAIRWISE_STREAMING_HLSLI
#define RESTIR_DI_PAIRWISE_STREAMING_HLSLI

#include "ReSTIR/DI/Reservoir.hlsli"

// A helper used for pairwise MIS computations.  This might be able to simplify code elsewhere, too.
float ComputeTargetPdfAtSurface(const ReSTIRDIReservoir lightReservoir, const DISurface surface, bool priorFrame RESTIR_DEFAULT(false))
{
    DILightSample lightSample = DISampleLight(
        DILoadLightInfo(GetDIReservoirLightIndex(lightReservoir), priorFrame),
        surface, GetDIReservoirSampleUv(lightReservoir));

    return DIGetLightSampleTargetPdf(lightSample, surface);
}

// "Pairwise MIS" is a MIS approach that is O(N) instead of O(N^2) for N estimators.  The idea is you know
// a canonical sample which is a known (pretty-)good estimator, but you'd still like to improve the result
// given multiple other candidate estimators.  You can do this in a pairwise fashion, MIS'ing between each
// candidate and the canonical sample.  StreamNeighborWithPairwiseMIS() is executed once for each 
// candidate, after which the MIS is completed by calling StreamCanonicalWithPairwiseStep() once for
// the canonical sample.
// See Chapter 9.1 of https://digitalcommons.dartmouth.edu/dissertations/77/, especially Eq 9.10 & Algo 8
bool StreamNeighborWithPairwiseMIS(inout ReSTIRDIReservoir reservoir,
    float random,
    const ReSTIRDIReservoir neighborReservoir,
    const DISurface neighborSurface,
    const ReSTIRDIReservoir canonicalReservor,
    const DISurface canonicalSurface,
    const uint numberOfNeighborsInStream)    // # neighbors streamed via pairwise MIS before streaming the canonical sample
{
    // Compute PDFs of the neighbor and cannonical light samples and surfaces in all permutations.
    // Note: First two must be computed this way.  Last two *should* be replacable by neighborReservoir.targetPdf
    // and canonicalReservor.targetPdf to reduce redundant computations, but there's a bug in that naive reuse.
    float neighborWeightAtCanonical = max(0.0f, ComputeTargetPdfAtSurface(neighborReservoir, canonicalSurface, false));
    float canonicalWeightAtNeighbor = max(0.0f, ComputeTargetPdfAtSurface(canonicalReservor, neighborSurface, false));
    float neighborWeightAtNeighbor = max(0.0f, ComputeTargetPdfAtSurface(neighborReservoir, neighborSurface, false));
    float canonicalWeightAtCanonical = max(0.0f, ComputeTargetPdfAtSurface(canonicalReservor, canonicalSurface, false));

    // Compute two pairwise MIS weights
    float w0 = ComputePairwiseMisWeight(neighborWeightAtNeighbor, neighborWeightAtCanonical,
        neighborReservoir.M * numberOfNeighborsInStream, canonicalReservor.M);
    float w1 = ComputePairwiseMisWeight(canonicalWeightAtNeighbor, canonicalWeightAtCanonical,
        neighborReservoir.M * numberOfNeighborsInStream, canonicalReservor.M);

    // Determine the effective M value when using pairwise MIS
    float M = neighborReservoir.M * min(
        ComputeMFactor(neighborWeightAtNeighbor, neighborWeightAtCanonical),
        ComputeMFactor(canonicalWeightAtNeighbor, canonicalWeightAtCanonical));

    // With pairwise MIS, we touch the canonical sample multiple times (but every other sample only once).  This 
    // with overweight the canonical sample; we track how much it is overweighted so we can renormalize to account
    // for this in the function StreamCanonicalWithPairwiseStep()
    reservoir.canonicalWeight += (1.0f - w1);

    // Go ahead and stream the neighbor sample through via RIS, appropriately weighted
    return InternalSimpleResample(reservoir, neighborReservoir, random,
        neighborWeightAtCanonical,
        neighborReservoir.weightSum * w0,
        M);
}

// Called to finish the process of doing pairwise MIS.  This function must be called after all required calls to
// StreamNeighborWithPairwiseMIS(), since pairwise MIS overweighs the canonical sample.  This function 
// compensates for this overweighting, but it can only happen after all neighbors have been processed.
bool StreamCanonicalWithPairwiseStep(inout ReSTIRDIReservoir reservoir,
    float random,
    const ReSTIRDIReservoir canonicalReservoir,
    const DISurface canonicalSurface)
{
    return InternalSimpleResample(reservoir, canonicalReservoir, random,
        canonicalReservoir.targetPdf,
        canonicalReservoir.weightSum * reservoir.canonicalWeight,
        canonicalReservoir.M);
}

#endif // RESTIR_DI_PAIRWISE_STREAMING_HLSLI
