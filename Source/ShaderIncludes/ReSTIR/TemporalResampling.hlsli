#ifndef RESTIR_DI_TEMPORAL_RESAMPLING_HLSLI
#define RESTIR_DI_TEMPORAL_RESAMPLING_HLSLI

#include "ReSTIR/Common.h.slang"
#include "ReSTIR/Random.hlsli"
#include "ReSTIR/Reservoir.hlsli"
#include <ReSTIR/ReservoirStorage.hlsli>

// Temporal resampling pass.
// Takes the previous G-buffer, motion vectors, and two light reservoir buffers as inputs.
// Tries to match the surfaces in the current frame to surfaces in the previous frame.
// If a match is found for a given pixel, the current and previous reservoirs are 
// combined. An optional visibility ray may be cast if enabled, to reduce the resampling bias.
// This project keeps one current TLAS. Temporal reuse therefore reprojects into
// the previous surface buffer, but any optional visibility check is approximate.
ReSTIRDIReservoir ReSTIRDIRunTemporalResampling(
    uint2 pixelPosition,
    DISurface surface,
    ReSTIRDIReservoir curSample,
    inout ReSTIRRandomSamplerState rng,
    ReSTIRReservoirBufferParameters reservoirParams,
	float3 screenSpaceMotion,
	uint sourceBufferIndex,
    ReSTIRDITemporalResamplingParameters tparams,
    out int2 temporalSamplePixelPos,
    out uint temporalStatus)
{
    // Do not let a single reused reservoir grow beyond the configured temporal history.
    uint historyLimit = min(ReSTIRPackedDIReservoirMaxM, uint(tparams.maxHistoryLength * curSample.M));

    int selectedLightPrevID = -1;

    // Keep the current light id around for the normalization pass.
    if (IsValidDIReservoir(curSample))
    {
        selectedLightPrevID = DITranslateLightIndex(GetDIReservoirLightIndex(curSample));
    }

    temporalSamplePixelPos = int2(-1, -1);
    temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone);

    ReSTIRDIReservoir state = EmptyDIReservoir();
    CombineDIReservoirs(state, curSample, /* random = */ 0.5, curSample.targetPdf);

    // Backproject this pixel to last frame.
    float3 motion = screenSpaceMotion;
    
    if (tparams.enablePermutationSampling == 0)
    {
        // Jitter the lookup when permutation sampling is disabled so temporal
        // failure patterns do not become a fixed screen-space grid.
        motion.xy += float2(GetNextRandom(rng), GetNextRandom(rng)) - 0.5;
    }

    // Motion is stored as "previous minus current" in pixel space.
    float2 reprojectedSamplePosition = float2(pixelPosition) + motion.xy;
    int2 prevPos = int2(round(reprojectedSamplePosition));

    // Z motion is the previous linear depth minus the current linear depth.
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
            // Permutation sampling moves the initial lookup in a reversible
            // 4x4 pattern, matching the ReSTIR paper's temporal decorrelation idea.
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
    // previousM is the candidate count represented by the previous-frame reservoir.
    float previousM = 0;

    if (!foundNeighbor)
    {
        temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface);
    }

    if (foundNeighbor)
    {
        // Resample the previous frame sample into the current reservoir, but reduce the light's weight
        // according to the bilinear weight of the current pixel
        uint2 prevReservoirPos = PixelPosToReservoirPos(prevPos);
        ReSTIRDIReservoir prevSample = LoadDIReservoir(reservoirParams,
            prevReservoirPos, sourceBufferIndex);

        // Clamp how much history the previous sample can bring forward.
        prevSample.M = min(prevSample.M, historyLimit);

        // Move the reservoir's stored visibility position along with the reprojection.
        prevSample.spatialDistance += spatialOffset;
        prevSample.age += 1;

        uint originalPrevLightID = GetDIReservoirLightIndex(prevSample);

        // Map the light ID from the previous frame into the current frame, if it still exists.
        bool hasUsablePreviousReservoir = false;

        if (IsValidDIReservoir(prevSample))
        {
            if (prevSample.age <= 1)
            {
                temporalSamplePixelPos = prevPos;
            }

            int mappedLightID = DITranslateLightIndex(GetDIReservoirLightIndex(prevSample));

            if (mappedLightID < 0)
            {
                // The selected light no longer exists, so this previous reservoir cannot be reused.
                prevSample.weightSum = 0;
                prevSample.lightData = 0;
            }
            else
            {
                // Store the current-frame light id before combining it into this frame's reservoir.
                prevSample.lightData = mappedLightID | ReSTIRDIReservoirLightValidBit;
                hasUsablePreviousReservoir = true;
            }
        }

        previousM = prevSample.M;

        float weightAtCurrent = 0;
        DILightSample candidateLightSample = DIEmptyLightSample();
        if (IsValidDIReservoir(prevSample))
        {
            const DILightInfo candidateLight = DILoadLightInfo(GetDIReservoirLightIndex(prevSample));

            // Re-evaluate the previous light sample at the current receiver.
            candidateLightSample = DISampleLight(
                candidateLight, surface, GetDIReservoirSampleUv(prevSample));

            weightAtCurrent = DIGetLightSampleTargetPdf(candidateLightSample, surface);
        }

        if (hasUsablePreviousReservoir && weightAtCurrent > 0.0)
        {
            temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);
        }
        else if (hasUsablePreviousReservoir)
        {
            temporalStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedTargetPdf);
        }

        bool sampleSelected = CombineDIReservoirs(state, prevSample, GetNextRandom(rng), weightAtCurrent);
        if(sampleSelected)
        {
            // The selected sample still needs to be evaluated at the previous
            // receiver during the MIS-like normalization step.
            selectedPreviousSample = true;
            selectedLightPrevID = int(originalPrevLightID);
        }
    }

#if RESTIR_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_BASIC
    if (tparams.biasCorrectionMode >= RESTIR_BIAS_CORRECTION_BASIC)
    {
        // Compute the MIS-like normalization term instead of using 1/M.
        // pi is the target PDF of the selected representative sample at the current receiver.
        float pi = state.targetPdf;
        // piSum accumulates target PDF times represented sample count across participating reservoirs.
        float piSum = state.targetPdf * curSample.M;
        
        if (IsValidDIReservoir(state) && selectedLightPrevID >= 0 && previousM > 0)
        {
            float temporalP = 0;

            const DILightInfo selectedLightPrev = DILoadLightInfo(selectedLightPrevID);

            // Re-evaluate the selected sample at the previous receiver for the normalization sum.
            const DILightSample selectedSampleAtTemporal = DISampleLight(
                selectedLightPrev, temporalSurface, GetDIReservoirSampleUv(state));
        
            temporalP = DIGetLightSampleTargetPdf(selectedSampleAtTemporal, temporalSurface);

#if RESTIR_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_RAY_TRACED
            if (tparams.biasCorrectionMode == RESTIR_BIAS_CORRECTION_RAY_TRACED && temporalP > 0
                && (!selectedPreviousSample || tparams.enableVisibilityShortcut == 0))
            {
                // This is an approximation from the current receiver because
                // the renderer keeps only the current TLAS.
                if (!DIGetTemporalConservativeVisibility(surface, selectedSampleAtTemporal))
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
        // Fast path for the intentionally biased 1/M normalization mode.
        FinalizeDIResampling(state, 1.0, state.M);
    }

    return state;
}

#endif // RESTIR_DI_TEMPORAL_RESAMPLING_HLSLI
