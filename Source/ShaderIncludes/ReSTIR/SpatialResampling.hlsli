#ifndef RESTIR_DI_SPATIAL_RESAMPLING_HLSLI
#define RESTIR_DI_SPATIAL_RESAMPLING_HLSLI

#include "ReSTIR/Common.h.slang"
#include "ReSTIR/Parameters.h"
#include <ReSTIR/ReservoirStorage.hlsli>

#ifndef RESTIR_NEIGHBOR_OFFSETS_BUFFER
#error "RESTIR_NEIGHBOR_OFFSETS_BUFFER must be defined to point to a Buffer<float2> type resource"
#endif

// This macro can be defined in the including shader file to reduce code bloat
// and/or remove ray tracing calls from temporal and spatial resampling shaders
// if bias correction is not necessary.
#ifndef RESTIR_ALLOWED_BIAS_CORRECTION
#define RESTIR_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_RAY_TRACED
#endif

// Spatial resampling pass.
// Operates on the current frame G-buffer and its reservoirs.
// For each pixel, considers a number of its neighbors and, if their surfaces are 
// similar enough to the current pixel, combines their light reservoirs.
// Optionally, one visibility ray is traced for each neighbor being considered, to reduce bias.
// The selectedLightSample parameter is used to update and return the selected sample; it's optional,
// and it's safe to pass a null structure there and ignore the result.
ReSTIRDIReservoir ReSTIRDIRunSpatialResampling(
    uint2 pixelPosition,
    DISurface centerSurface,
    ReSTIRDIReservoir centerSample,
    inout ReSTIRRandomSamplerState rng,
    ReSTIRRuntimeParameters params,
    ReSTIRReservoirBufferParameters reservoirParams,
    uint sourceBufferIndex,
    ReSTIRDISpatialResamplingParameters sparams,
    inout DILightSample selectedLightSample)
{
    ReSTIRDIReservoir state = EmptyDIReservoir();

    // This is the weight we'll use (instead of 1/M) to make our estimate unbaised (see paper).
    float normalizationWeight = 1.0f;

    // Since we're using our bias correction scheme, we need to remember which light selection we made
    int selected = -1;

    DILightInfo selectedLight = DIEmptyLightInfo();

    if (IsValidDIReservoir(centerSample))
    {
        selectedLight = DILoadLightInfo(GetDIReservoirLightIndex(centerSample), false);
    }

    CombineDIReservoirs(state, centerSample, /* random = */ 0.5f, centerSample.targetPdf);

    uint startIdx = uint(GetNextRandom(rng) * params.neighborOffsetMask);
    
    uint i;
    uint numSpatialSamples = sparams.numSamples;
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
        // Get screen-space location of neighbor
        uint sampleIdx = (startIdx + i) & params.neighborOffsetMask;
        int2 spatialOffset = int2(float2(RESTIR_NEIGHBOR_OFFSETS_BUFFER[sampleIdx].xy) * sparams.samplingRadius);
        int2 idx = int2(pixelPosition) + spatialOffset;

        idx = DIClampSamplePositionIntoView(idx, false);

        DISurface neighborSurface = DILoadGBufferSurface(idx, false);

        if (!DIIsSurfaceValid(neighborSurface))
            continue;

        if (!IsValidNeighbor(DIGetSurfaceNormal(centerSurface), DIGetSurfaceNormal(neighborSurface), 
            DIGetSurfaceLinearDepth(centerSurface), DIGetSurfaceLinearDepth(neighborSurface), 
            sparams.normalThreshold, sparams.depthThreshold))
            continue;

        if (sparams.enableMaterialSimilarityTest != 0 && !DIAreMaterialsSimilar(DIGetMaterial(centerSurface), DIGetMaterial(neighborSurface)))
            continue;

        uint2 neighborReservoirPos = PixelPosToReservoirPos(idx);

        ReSTIRDIReservoir neighborSample = LoadDIReservoir(reservoirParams,
            neighborReservoirPos, sourceBufferIndex);
        neighborSample.spatialDistance += spatialOffset;

        cachedResult |= (1u << uint(i));

        DILightInfo candidateLight = DIEmptyLightInfo();

        // Load that neighbor's RIS state, do resampling
        float neighborWeight = 0;
        DILightSample candidateLightSample = DIEmptyLightSample();
        if (IsValidDIReservoir(neighborSample))
        {   
            if (sparams.discountNaiveSamples != 0 && neighborSample.M <= RESTIR_NAIVE_SAMPLING_M_THRESHOLD)
                continue;

            candidateLight = DILoadLightInfo(GetDIReservoirLightIndex(neighborSample), false);
            
            candidateLightSample = DISampleLight(
                candidateLight, centerSurface, GetDIReservoirSampleUv(neighborSample));
            
            neighborWeight = DIGetLightSampleTargetPdf(candidateLightSample, centerSurface);
        }
        
        if (CombineDIReservoirs(state, neighborSample, GetNextRandom(rng), neighborWeight))
        {
            selected = int(i);
            selectedLight = candidateLight;
            selectedLightSample = candidateLightSample;
        }
    }

    if (IsValidDIReservoir(state))
    {
#if RESTIR_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_BASIC
        if (sparams.biasCorrectionMode >= RESTIR_BIAS_CORRECTION_BASIC)
        {
            // Compute the unbiased normalization term (instead of using 1/M)
            float pi = state.targetPdf;
            float piSum = state.targetPdf * centerSample.M;

            // To do this, we need to walk our neighbors again
            for (i = 0; i < numSpatialSamples; ++i)
            {
                // If we skipped this neighbor above, do so again.
                if ((cachedResult & (1u << uint(i))) == 0) continue;

                uint sampleIdx = (startIdx + i) & params.neighborOffsetMask;

                // Get the screen-space location of our neighbor
                int2 idx = int2(pixelPosition) + int2(float2(RESTIR_NEIGHBOR_OFFSETS_BUFFER[sampleIdx].xy) * sparams.samplingRadius);

                idx = DIClampSamplePositionIntoView(idx, false);

                // Load our neighbor's G-buffer
                DISurface neighborSurface = DILoadGBufferSurface(idx, false);
                
                // Get the PDF of the sample RIS selected in the first loop, above, *at this neighbor* 
                const DILightSample selectedSampleAtNeighbor = DISampleLight(
                    selectedLight, neighborSurface, GetDIReservoirSampleUv(state));

                float ps = DIGetLightSampleTargetPdf(selectedSampleAtNeighbor, neighborSurface);

#if RESTIR_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_RAY_TRACED
                if (sparams.biasCorrectionMode == RESTIR_BIAS_CORRECTION_RAY_TRACED && ps > 0)
                {
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
            FinalizeDIResampling(state, 1.0, state.M);
        }
    }

    return state;
}

#endif // RESTIR_DI_SPATIAL_RESAMPLING_HLSLI
