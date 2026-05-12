#ifndef RESTIR_DI_TEMPORAL_RESAMPLING_HLSLI
#define RESTIR_DI_TEMPORAL_RESAMPLING_HLSLI

#include "ReSTIR/Common.h.slang"
#include "ReSTIR/Reservoir.hlsli"
#include <ReSTIR/ReservoirStorage.hlsli>

// Temporal resampling pass.
// Takes the previous G-buffer, motion vectors, and two light reservoir buffers as inputs.
// Tries to match the surfaces in the current frame to surfaces in the previous frame.
// If a match is found for a given pixel, the current and previous reservoirs are 
// combined. An optional visibility ray may be cast if enabled, to reduce the resampling bias.
// That visibility ray should ideally be traced through the previous frame BVH, but
// can also use the current frame BVH if the previous is not available - that will produce more bias.
// The selectedLightSample parameter is used to update and return the selected sample; it's optional,
// and it's safe to pass a null structure there and ignore the result.
ReSTIRDIReservoir ReSTIRDIRunTemporalResampling(
    uint2 pixelPosition,
    DISurface surface,
    ReSTIRDIReservoir curSample,
    inout ReSTIRRandomSamplerState rng,
    ReSTIRRuntimeParameters params,
    ReSTIRReservoirBufferParameters reservoirParams,
	float3 screenSpaceMotion,
	uint sourceBufferIndex,
    ReSTIRDITemporalResamplingParameters tparams,
    out int2 temporalSamplePixelPos,
    inout DILightSample selectedLightSample)
{
    uint historyLimit = min(ReSTIRPackedDIReservoirMaxM, uint(tparams.maxHistoryLength * curSample.M));

    int selectedLightPrevID = -1;

    if (IsValidDIReservoir(curSample))
    {
        selectedLightPrevID = DITranslateLightIndex(GetDIReservoirLightIndex(curSample), true);
    }

    temporalSamplePixelPos = int2(-1, -1);

    ReSTIRDIReservoir state = EmptyDIReservoir();
    CombineDIReservoirs(state, curSample, /* random = */ 0.5, curSample.targetPdf);

    // Backproject this pixel to last frame
    float3 motion = screenSpaceMotion;
    
    if (tparams.enablePermutationSampling == 0)
    {
        motion.xy += float2(GetNextRandom(rng), GetNextRandom(rng)) - 0.5;
    }

    float2 reprojectedSamplePosition = float2(pixelPosition) + motion.xy;
    int2 prevPos = int2(round(reprojectedSamplePosition));

    float expectedPrevLinearDepth = DIGetSurfaceLinearDepth(surface) + motion.z;

    DISurface temporalSurface = DIEmptySurface();
    bool foundNeighbor = false;
    const float radius = 4;
    int2 spatialOffset = int2(0, 0);

    // Try to find a matching surface in the neighborhood of the reprojected pixel
    for(int i = 0; i < 9; i++)
    {
        int2 offset = int2(0, 0);
        if(i > 0)
        {
            offset.x = int((GetNextRandom(rng) - 0.5) * radius);
            offset.y = int((GetNextRandom(rng) - 0.5) * radius);
        }

        int2 idx = prevPos + offset;
        if (tparams.enablePermutationSampling != 0 && i == 0)
        {
            ApplyPermutationSampling(idx, tparams.uniformRandomNumber);
        }

        // Grab shading / g-buffer data from last frame
        temporalSurface = DILoadGBufferSurface(idx, true);
        if (!DIIsSurfaceValid(temporalSurface))
            continue;
        
        // Test surface similarity, discard the sample if the surface is too different.
        if (!IsValidNeighbor(
            DIGetSurfaceNormal(surface), DIGetSurfaceNormal(temporalSurface), 
            expectedPrevLinearDepth, DIGetSurfaceLinearDepth(temporalSurface), 
            tparams.normalThreshold, tparams.depthThreshold))
            continue;

        spatialOffset = idx - prevPos;
        prevPos = idx;
        foundNeighbor = true;

        break;
    }

    bool selectedPreviousSample = false;
    float previousM = 0;

    if (foundNeighbor)
    {
        // Resample the previous frame sample into the current reservoir, but reduce the light's weight
        // according to the bilinear weight of the current pixel
        uint2 prevReservoirPos = PixelPosToReservoirPos(prevPos);
        ReSTIRDIReservoir prevSample = LoadDIReservoir(reservoirParams,
            prevReservoirPos, sourceBufferIndex);
        prevSample.M = min(prevSample.M, historyLimit);
        prevSample.spatialDistance += spatialOffset;
        prevSample.age += 1;

        uint originalPrevLightID = GetDIReservoirLightIndex(prevSample);

        // Map the light ID from the previous frame into the current frame, if it still exists
        if (IsValidDIReservoir(prevSample))
        {
            if (prevSample.age <= 1)
            {
                temporalSamplePixelPos = prevPos;
            }

            int mappedLightID = DITranslateLightIndex(GetDIReservoirLightIndex(prevSample), false);

            if (mappedLightID < 0)
            {
                // Kill the reservoir
                prevSample.weightSum = 0;
                prevSample.lightData = 0;
            }
            else
            {
                // Sample is valid - modify the light ID stored
                prevSample.lightData = mappedLightID | ReSTIRDIReservoirLightValidBit;
            }
        }

        previousM = prevSample.M;

        float weightAtCurrent = 0;
        DILightSample candidateLightSample = DIEmptyLightSample();
        if (IsValidDIReservoir(prevSample))
        {
            const DILightInfo candidateLight = DILoadLightInfo(GetDIReservoirLightIndex(prevSample), false);

            candidateLightSample = DISampleLight(
                candidateLight, surface, GetDIReservoirSampleUv(prevSample));

            weightAtCurrent = DIGetLightSampleTargetPdf(candidateLightSample, surface);
        }

        bool sampleSelected = CombineDIReservoirs(state, prevSample, GetNextRandom(rng), weightAtCurrent);
        if(sampleSelected)
        {
            selectedPreviousSample = true;
            selectedLightPrevID = int(originalPrevLightID);
            selectedLightSample = candidateLightSample;
        }
    }

#if RESTIR_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_BASIC
    if (tparams.biasCorrectionMode >= RESTIR_BIAS_CORRECTION_BASIC)
    {
        // Compute the unbiased normalization term (instead of using 1/M)
        float pi = state.targetPdf;
        float piSum = state.targetPdf * curSample.M;
        
        if (IsValidDIReservoir(state) && selectedLightPrevID >= 0 && previousM > 0)
        {
            float temporalP = 0;

            const DILightInfo selectedLightPrev = DILoadLightInfo(selectedLightPrevID, true);

            // Get the PDF of the sample RIS selected in the first loop, above, *at this neighbor* 
            const DILightSample selectedSampleAtTemporal = DISampleLight(
                selectedLightPrev, temporalSurface, GetDIReservoirSampleUv(state));
        
            temporalP = DIGetLightSampleTargetPdf(selectedSampleAtTemporal, temporalSurface);

#if RESTIR_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_RAY_TRACED
            if (tparams.biasCorrectionMode == RESTIR_BIAS_CORRECTION_RAY_TRACED && temporalP > 0
                && (!selectedPreviousSample || tparams.enableVisibilityShortcut == 0))
            {
                if (!DIGetTemporalConservativeVisibility(surface, temporalSurface, selectedSampleAtTemporal))
                {
                    temporalP = 0;
                }
            }
#endif

            pi = selectedPreviousSample ? temporalP : pi;
            piSum += temporalP * previousM;
        }

        FinalizeDIResampling(state, pi, piSum);
    }
    else
#endif
    {
        FinalizeDIResampling(state, 1.0, state.M);
    }

    return state;
}

#endif // RESTIR_DI_TEMPORAL_RESAMPLING_HLSLI
