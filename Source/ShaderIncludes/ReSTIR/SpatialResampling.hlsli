#ifndef RESTIR_DI_SPATIAL_RESAMPLING_HLSLI
#define RESTIR_DI_SPATIAL_RESAMPLING_HLSLI

#include "ReSTIR/Common.h.slang"
#include "ReSTIR/Parameters.h"
#include "ReSTIR/Random.hlsli"
#include <ReSTIR/ReservoirStorage.hlsli>

#ifndef RESTIR_NEIGHBOR_OFFSETS_BUFFER
#error "RESTIR_NEIGHBOR_OFFSETS_BUFFER must be defined to point to a StructuredBuffer<ReSTIRNeighborOffset> type resource"
#endif

float2 LoadReSTIRNeighborOffset(uint sampleIdx)
{
    return RESTIR_NEIGHBOR_OFFSETS_BUFFER[sampleIdx].offset;
}

// The renderer keeps ray-traced bias correction enabled in the ReSTIR
// passes. The macro is still used by the included reservoir code to compile the
// relevant normalization path.
#ifndef RESTIR_ALLOWED_BIAS_CORRECTION
#define RESTIR_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_RAY_TRACED
#endif

// Spatial resampling pass.
// Operates on the current frame G-buffer and its reservoirs.
// For each pixel, considers a number of its neighbors and, if their surfaces are 
// similar enough to the current pixel, combines their light reservoirs.
// Optionally, one visibility ray is traced for each neighbor being considered, to reduce bias.
ReSTIRDIReservoir ReSTIRDIRunSpatialResampling(
    uint2 pixelPosition,
    DISurface centerSurface,
    ReSTIRDIReservoir centerSample,
    inout ReSTIRRandomSamplerState rng,
    ReSTIRRuntimeParameters params,
    ReSTIRReservoirBufferParameters reservoirParams,
    uint sourceBufferIndex,
    ReSTIRDISpatialResamplingParameters sparams)
{
    ReSTIRDIReservoir state = EmptyDIReservoir();

    // Bias correction must later know which neighbor supplied the selected sample.
    int selected = -1;

    DILightInfo selectedLight = DIEmptyLightInfo();

    // The center sample is always part of the candidate stream.
    if (IsValidDIReservoir(centerSample))
    {
        selectedLight = DILoadLightInfo(GetDIReservoirLightIndex(centerSample));
    }

    CombineDIReservoirs(state, centerSample, /* random = */ 0.5f, centerSample.targetPdf);

    uint neighborOffsetCount = params.neighborOffsetMask + 1u;
    uint startIdx = uint(GetNextRandom(rng) * float(neighborOffsetCount)) & params.neighborOffsetMask;
    
    uint i;
    uint numSpatialSamples = sparams.numSamples;
    // New or disoccluded pixels have weak history, so they get extra neighbors.
    if(centerSample.M < sparams.targetHistoryLength)
        numSpatialSamples = max(sparams.numDisocclusionBoostSamples, numSpatialSamples);

    // Clamp the sample count at 32 to make sure we can keep the neighbor mask in an uint (cachedResult)
    numSpatialSamples = min(numSpatialSamples, 32);

    // We loop through neighbors twice.  Cache the validity / edge-stopping function
    //   results for the 2nd time through.
    uint cachedResult = 0;

    // Walk the specified number of neighbors, resampling using RIS
    for (i = 0; i < numSpatialSamples; ++i)
    {
        // Rotate through the fixed neighbor pattern so pixels do not all sample
        // the same surrounding locations.
        uint sampleIdx = (startIdx + i) & params.neighborOffsetMask;
        int2 spatialOffset = int2(LoadReSTIRNeighborOffset(sampleIdx) * sparams.samplingRadius);
        int2 idx = int2(pixelPosition) + spatialOffset;

        // Mirror out-of-bounds samples back into view to avoid edge clumping.
        idx = DIClampSamplePositionIntoView(idx);

        // Neighbor reuse only makes sense if the current pixel and neighbor see
        // roughly the same local surface.
        DISurface neighborSurface = DILoadGBufferSurface(idx, false);

        if (!DIIsSurfaceValid(neighborSurface))
            continue;

        if (!IsValidNeighbor(DIGetSurfaceNormal(centerSurface), DIGetSurfaceNormal(neighborSurface), 
            DIGetSurfaceLinearDepth(centerSurface), DIGetSurfaceLinearDepth(neighborSurface), 
            sparams.normalThreshold, sparams.depthThreshold))
            continue;

        if (sparams.enableMaterialSimilarityTest != 0 && !DIAreMaterialsSimilar(DIGetMaterial(centerSurface), DIGetMaterial(neighborSurface)))
            continue;

        // Pull the neighbor's reservoir from the current source buffer.
        uint2 neighborReservoirPos = PixelPosToReservoirPos(idx);

        ReSTIRDIReservoir neighborSample = LoadDIReservoir(reservoirParams,
            neighborReservoirPos, sourceBufferIndex);

        // Track how far this visibility information has moved on screen.
        neighborSample.spatialDistance += spatialOffset;

        DILightInfo candidateLight = DIEmptyLightInfo();

        // Load that neighbor's RIS state, do resampling
        float neighborWeight = 0;
        DILightSample candidateLightSample = DIEmptyLightSample();
        if (IsValidDIReservoir(neighborSample))
        {   
            // Avoid spreading very fresh one-frame reservoirs through space.
            if (sparams.discountNaiveSamples != 0 && neighborSample.M <= RESTIR_NAIVE_SAMPLING_M_THRESHOLD)
                continue;

            candidateLight = DILoadLightInfo(GetDIReservoirLightIndex(neighborSample));
            
            // Re-evaluate the neighbor's light sample at the center surface,
            // because the selected light may be good for the neighbor and bad
            // for this pixel.
            candidateLightSample = DISampleLight(
                candidateLight, centerSurface, GetDIReservoirSampleUv(neighborSample));
            
            neighborWeight = DIGetLightSampleTargetPdf(candidateLightSample, centerSurface);
        }

        // The cached bit means "this neighbor participated in resampling".
        // The normalization pass below uses the same mask to decide which M
        // values belong in the MIS-like denominator.
        cachedResult |= (1u << uint(i));
        
        if (CombineDIReservoirs(state, neighborSample, GetNextRandom(rng), neighborWeight))
        {
            selected = int(i);
            selectedLight = candidateLight;
        }
    }

    if (IsValidDIReservoir(state))
    {
#if RESTIR_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_BASIC
        if (sparams.biasCorrectionMode >= RESTIR_BIAS_CORRECTION_BASIC)
        {
            // Compute the MIS-like normalization term instead of using 1/M.
            float pi = state.targetPdf;
            float piSum = state.targetPdf * centerSample.M;

            // To do this, we need to walk our neighbors again
            for (i = 0; i < numSpatialSamples; ++i)
            {
                // If we skipped this neighbor above, do so again.
                if ((cachedResult & (1u << uint(i))) == 0) continue;

                uint sampleIdx = (startIdx + i) & params.neighborOffsetMask;

                // Get the screen-space location of our neighbor
                int2 idx = int2(pixelPosition) + int2(LoadReSTIRNeighborOffset(sampleIdx) * sparams.samplingRadius);

                idx = DIClampSamplePositionIntoView(idx);

                // Load our neighbor's G-buffer
                DISurface neighborSurface = DILoadGBufferSurface(idx, false);
                
                // Get the PDF of the sample RIS selected in the first loop, above, *at this neighbor* 
                const DILightSample selectedSampleAtNeighbor = DISampleLight(
                    selectedLight, neighborSurface, GetDIReservoirSampleUv(state));

                float ps = DIGetLightSampleTargetPdf(selectedSampleAtNeighbor, neighborSurface);

#if RESTIR_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_RAY_TRACED
                if (sparams.biasCorrectionMode == RESTIR_BIAS_CORRECTION_RAY_TRACED && ps > 0)
                {
                    // Visibility is tested from the neighbor receiver when
                    // contributing that neighbor's M to the normalization sum.
                    if (!DIGetConservativeVisibility(neighborSurface, selectedSampleAtNeighbor))
                    {
                        ps = 0;
                    }
                }
#endif

                uint2 neighborReservoirPos = PixelPosToReservoirPos(idx);

                ReSTIRDIReservoir neighborSample = LoadDIReservoir(reservoirParams,
                    neighborReservoirPos, sourceBufferIndex);

                // Select this sample for the (normalization) numerator if this particular neighbor pixel
                //     was the one we selected via RIS in the first loop, above.
                pi = selected == i ? ps : pi;

                // Add to the sums of weights for the (normalization) denominator
                piSum += ps * neighborSample.M;
            }

            // Use "MIS-like" normalization
            FinalizeDIResampling(state, pi, piSum);
        }
        else
#endif
        {
            // Fast path for the intentionally biased 1/M normalization mode.
            FinalizeDIResampling(state, 1.0, state.M);
        }
    }

    return state;
}

#endif // RESTIR_DI_SPATIAL_RESAMPLING_HLSLI
