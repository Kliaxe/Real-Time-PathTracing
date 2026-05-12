#ifndef INITIAL_SAMPLING_FUNCTIONS_HLSLI
#define INITIAL_SAMPLING_FUNCTIONS_HLSLI

#include "ReSTIR/Parameters.h"
#include "ReSTIR/Reservoir.hlsli"

#define RESTIR_DI_STRATIFY_LOCAL_SAMPLING 1

//
// MIS functions
//

struct DIInitialSamplingMisData
{
	uint numMisSamples;
	float localLightMisWeight;
	float environmentMapMisWeight;
	float brdfMisWeight;
};

DIInitialSamplingMisData ComputeEffectiveInitialSamplingMisData(
    ReSTIRDIInitialSamplingParameters initialSamplingParams,
    ReSTIRLightBufferParameters lightBufferParams)
{
    DIInitialSamplingMisData result;

    const uint effectiveLocalLightSamples =
        lightBufferParams.localLightBufferRegion.numLights > 0 ? initialSamplingParams.numLocalLightSamples : 0u;
    const uint effectiveEnvironmentSamples =
        lightBufferParams.environmentLightParams.lightPresent != 0 ? initialSamplingParams.numEnvironmentSamples : 0u;
    const uint effectiveBrdfSamples = initialSamplingParams.numBrdfSamples;

    result.numMisSamples = max(1u, effectiveLocalLightSamples + effectiveEnvironmentSamples + effectiveBrdfSamples);
    result.localLightMisWeight = float(effectiveLocalLightSamples) / float(result.numMisSamples);
    result.environmentMapMisWeight = float(effectiveEnvironmentSamples) / float(result.numMisSamples);
    result.brdfMisWeight = float(effectiveBrdfSamples) / float(result.numMisSamples);

    return result;
}

// Heuristic to determine a max visibility ray length from a PDF wrt. solid angle.
float ComputeBrdfMaxDistanceFromPdf(float brdfCutoff, float pdf)
{
    const float kRayTMax = 3.402823466e+38F; // FLT_MAX
    return brdfCutoff > 0.f ? sqrt((1.f / brdfCutoff - 1.f) * pdf) : kRayTMax;
}

// Computes the multi importance sampling pdf for brdf and light sample.
// For light and BRDF PDFs wrt solid angle, blend between the two.
//      lightSelectionPdf is a dimensionless selection pdf
float ComputeLightBrdfMisWeight(DISurface surface, DILightSample lightSample,
    float lightSelectionPdf, float lightMisWeight, bool isEnvironmentMap,
    float brdfMisWeight, float brdfCutoff)
{
    float lightSolidAnglePdf = DIGetLightSampleSolidAnglePdf(lightSample);
    if (brdfMisWeight == 0 || DIIsAnalyticLightSample(lightSample) ||
        lightSolidAnglePdf <= 0 || isinf(lightSolidAnglePdf) || isnan(lightSolidAnglePdf))
    {
        // BRDF samples disabled or we can't trace BRDF rays MIS with analytical lights
        return lightMisWeight * lightSelectionPdf;
    }

    float3 lightDir;
    float lightDistance;
    DIGetLightDirectionAndDistance(surface, lightSample, lightDir, lightDistance);

    // Compensate for ray shortening due to brdf cutoff, does not apply to environment map sampling
    float brdfPdf = DIEvaluateSurfaceBrdfPdf(surface, lightDir);
    float maxDistance = ComputeBrdfMaxDistanceFromPdf(brdfCutoff, brdfPdf);
    if (!isEnvironmentMap && lightDistance > maxDistance)
        brdfPdf = 0.f;

    // Convert light selection pdf (unitless) to a solid angle measurement
    float sourcePdfWrtSolidAngle = lightSelectionPdf * lightSolidAnglePdf;

    // MIS blending against solid angle pdfs.
    float blendedPdfWrtSolidangle = lightMisWeight * sourcePdfWrtSolidAngle + brdfMisWeight * brdfPdf;

    // Convert back, the DI resolve divides shading again by this term later
    return blendedPdfWrtSolidangle / lightSolidAnglePdf;
}

//
// Local light UV selection and reservoir streaming
//

struct LocalLightSelectionContext
{
    ReSTIRLightBufferRegion lightBufferRegion;
};

LocalLightSelectionContext InitializeLocalLightSelectionContext(ReSTIRLightBufferRegion lightBufferRegion)
{
    LocalLightSelectionContext ctx;
    ctx.lightBufferRegion = lightBufferRegion;
    return ctx;
}

void SelectNextLocalLight(
    LocalLightSelectionContext ctx,
    float rnd,
    out DILightInfo lightInfo,
    out uint lightIndex,
    out float invSourcePdf)
{
    invSourcePdf = float(ctx.lightBufferRegion.numLights);
    lightIndex = ctx.lightBufferRegion.firstLightIndex + min(uint(floor(rnd * ctx.lightBufferRegion.numLights)), ctx.lightBufferRegion.numLights - 1);
    lightInfo = DILoadLightInfo(lightIndex, false);
}

float2 SelectLocalLightUv(inout ReSTIRRandomSamplerState rng)
{
    float2 uv;
    uv.x = GetNextRandom(rng);
    uv.y = GetNextRandom(rng);
    return uv;
}

bool StreamLocalLightAtUvIntoReservoir(
    inout ReSTIRRandomSamplerState rng,
    DIInitialSamplingMisData misData,
    DISurface surface,
	float brdfCutoff,
	float localLightMisWeight,
    uint lightIndex,
    float2 uv,
    float invSourcePdf,
    DILightInfo lightInfo,
    inout ReSTIRDIReservoir state,
    inout DILightSample o_selectedSample)
{
    DILightSample candidateSample = DISampleLight(lightInfo, surface, uv);
    float blendedSourcePdf = ComputeLightBrdfMisWeight(surface, candidateSample, 1.0 / invSourcePdf,
        misData.localLightMisWeight, false, misData.brdfMisWeight, brdfCutoff);
    float targetPdf = DIGetLightSampleTargetPdf(candidateSample, surface);
    float risRnd = GetNextRandom(rng);

    if (blendedSourcePdf == 0)
    {
        return false;
    }
    bool selected = StreamDIReservoirSample(state, lightIndex, uv, risRnd, targetPdf, 1.0 / blendedSourcePdf);

    if (selected) {
        o_selectedSample = candidateSample;
    }
    return true;
}

ReSTIRDIReservoir SampleLocalLightsInternal(
    inout ReSTIRRandomSamplerState rng,
    DISurface surface,
    ReSTIRDIInitialSamplingParameters sampleParams,
	DIInitialSamplingMisData misData,
    ReSTIRLightBufferRegion localLightBufferRegion,
    out DILightSample o_selectedSample)
{
    ReSTIRDIReservoir state = EmptyDIReservoir();

    LocalLightSelectionContext lightSelectionContext = InitializeLocalLightSelectionContext(localLightBufferRegion);

    for (uint i = 0; i < sampleParams.numLocalLightSamples; i++)
    {
        uint lightIndex;
        DILightInfo lightInfo;
        float invSourcePdf;

        float rnd = GetNextRandom(rng);
#if RESTIR_DI_STRATIFY_LOCAL_SAMPLING
        rnd = (rnd + i) / sampleParams.numLocalLightSamples;
#endif // RESTIR_DI_STRATIFY_LOCAL_SAMPLING

        SelectNextLocalLight(lightSelectionContext, rnd, lightInfo, lightIndex, invSourcePdf);
        float2 uv = SelectLocalLightUv(rng);
        bool zeroPdf = StreamLocalLightAtUvIntoReservoir(rng, misData, surface, sampleParams.brdfCutoff, misData.localLightMisWeight, lightIndex, uv, invSourcePdf, lightInfo, state, o_selectedSample);

        if (zeroPdf)
            continue;
    }

    FinalizeDIResampling(state, 1.0, misData.numMisSamples);
    state.M = 1;

    return state;
}

//
// Local light sampling
//

ReSTIRDIReservoir SampleLocalLights(
    inout ReSTIRRandomSamplerState rng,
    DISurface surface,
    ReSTIRDIInitialSamplingParameters sampleParams,
	DIInitialSamplingMisData misData,
    ReSTIRLightBufferRegion localLightBufferRegion,
    out DILightSample o_selectedSample)
{
    o_selectedSample = DIEmptyLightSample();

    if (localLightBufferRegion.numLights == 0)
        return EmptyDIReservoir();

    if (sampleParams.numLocalLightSamples == 0)
        return EmptyDIReservoir();

    return SampleLocalLightsInternal(rng, surface, sampleParams, misData, localLightBufferRegion, o_selectedSample);
}

void StreamEnvironmentLightIntoReservoir(
    inout ReSTIRRandomSamplerState rng,
    ReSTIRDIInitialSamplingParameters sampleParams,
    DIInitialSamplingMisData misData,
    DISurface surface,
    DILightInfo lightInfo,
    uint environmentLightIndex,
    inout ReSTIRDIReservoir state,
    inout DILightSample o_selectedSample)
{
    float2 uv = float2(GetNextRandom(rng), GetNextRandom(rng));
    if(sampleParams.environmentMapImportanceSampling != 0)
    {
        const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
        if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0 && sceneInfo.environmentWidth > 0
           && sceneInfo.environmentHeight > 0)
        {
            const uint texelCount = sceneInfo.environmentWidth * sceneInfo.environmentHeight;
            const uint index = BinarySearchCdf(sceneInfo.environmentCdf, texelCount, GetNextRandom(rng));
            const uint x = index % sceneInfo.environmentWidth;
            const uint y = index / sceneInfo.environmentWidth;
            uv = (float2(float(x), float(y)) + frac(uv)) / float2(float(sceneInfo.environmentWidth), float(sceneInfo.environmentHeight));
        }
        else
        {
            float pdf = 0.0;
            const float3 sampledDirection = SampleCosineHemisphereDirection(surface.shadingNormal, frac(uv), pdf);
            uv = dirToLatLongUv(sampledDirection);
        }
    }

    DILightSample candidateSample = DISampleLight(lightInfo, surface, uv);
    float targetPdf = DIGetLightSampleTargetPdf(candidateSample, surface);
    float lightSelectionPdf = DIEvaluateEnvironmentMapSamplingPdf(candidateSample.direction);
    float blendedSourcePdf = ComputeLightBrdfMisWeight(surface, candidateSample, lightSelectionPdf,
        misData.environmentMapMisWeight, true, misData.brdfMisWeight, sampleParams.brdfCutoff);
    if(blendedSourcePdf <= 0.0)
    {
        return;
    }

    float risRnd = GetNextRandom(rng);
    bool selected = StreamDIReservoirSample(state, environmentLightIndex, uv, risRnd, targetPdf, 1.0 / blendedSourcePdf);
    if(selected)
    {
        o_selectedSample = candidateSample;
    }
}

ReSTIRDIReservoir SampleEnvironmentLight(
    inout ReSTIRRandomSamplerState rng,
    DISurface surface,
    ReSTIRDIInitialSamplingParameters sampleParams,
    DIInitialSamplingMisData misData,
    ReSTIREnvironmentLightBufferParameters params,
    out DILightSample o_selectedSample)
{
    ReSTIRDIReservoir state = EmptyDIReservoir();
    o_selectedSample = DIEmptyLightSample();

    if(params.lightPresent == 0 || sampleParams.numEnvironmentSamples == 0)
        return state;

    DILightInfo lightInfo = DILoadLightInfo(params.lightIndex, false);
    for(uint i = 0; i < sampleParams.numEnvironmentSamples; ++i)
    {
        StreamEnvironmentLightIntoReservoir(rng, sampleParams, misData, surface, lightInfo, params.lightIndex, state, o_selectedSample);
    }

    FinalizeDIResampling(state, 1.0, misData.numMisSamples);
    state.M = 1;
    return state;
}

//
// BRDF sampling: Samples from the BRDF defined by the given surface
//

ReSTIRDIReservoir SampleBrdfCandidates(
    inout ReSTIRRandomSamplerState rng,
    DISurface surface,
	uint numBrdfSamples,
	float brdfCutoff,
	float brdfRayMinT,
	DIInitialSamplingMisData misData,
	inout ReSTIRRandomSamplerState coherentRng,
    ReSTIRLightBufferParameters lightBufferParams,
    out DILightSample o_selectedSample)
{
    ReSTIRDIReservoir state = EmptyDIReservoir();
    
    for (uint i = 0; i < numBrdfSamples; ++i)
    {
        float lightSourcePdf = 0;
        float3 sampleDir;
        uint lightIndex = ReSTIRInvalidLightIndex;
        float2 randXY = float2(0, 0);
        DILightSample candidateSample = DIEmptyLightSample();

        if (DISampleSurfaceBrdf(surface, rng, sampleDir))
        {
            float brdfPdf = DIEvaluateSurfaceBrdfPdf(surface, sampleDir);
            float maxDistance = ComputeBrdfMaxDistanceFromPdf(brdfCutoff, brdfPdf);
            
            const float3 rayOrigin = OffsetRay(DIGetSurfaceWorldPosition(surface), SelectOffsetNormal(DIGetSurfaceGeometricNormal(surface), sampleDir));
            bool hitAnything = DITraceRayForLocalLight(rayOrigin, sampleDir,
                brdfRayMinT, maxDistance, lightIndex, randXY);

            if (lightIndex != ReSTIRInvalidLightIndex)
            {
                DILightInfo lightInfo = DILoadLightInfo(lightIndex, false);
                candidateSample = DISampleLight(lightInfo, surface, randXY);
                    
                if (brdfCutoff > 0.f)
                {
                    // If Mis cutoff is used, we need to evaluate the sample and make sure it actually could have been
                    // generated by the area sampling technique. This is due to numerical precision.
                    float3 lightDir;
                    float lightDistance;
                    DIGetLightDirectionAndDistance(surface, candidateSample, lightDir, lightDistance);

                    float brdfPdf = DIEvaluateSurfaceBrdfPdf(surface, lightDir);
                    float maxDistance = ComputeBrdfMaxDistanceFromPdf(brdfCutoff, brdfPdf);
                    if (lightDistance > maxDistance)
                        lightIndex = ReSTIRInvalidLightIndex;
                }

                if (lightIndex != ReSTIRInvalidLightIndex)
                {
                    lightSourcePdf = DIEvaluateLocalLightSourcePdf(lightIndex);
                }
            }
            else if (!hitAnything && (lightBufferParams.environmentLightParams.lightPresent != 0))
            {
                // sample environment light
                lightIndex = lightBufferParams.environmentLightParams.lightIndex;
                DILightInfo lightInfo = DILoadLightInfo(lightIndex, false);
                randXY = DIGetEnvironmentMapUvFromDirection(sampleDir);
                candidateSample = DISampleLight(lightInfo, surface, randXY);
                lightSourcePdf = DIEvaluateEnvironmentMapSamplingPdf(sampleDir);
            }
        }

        if (lightSourcePdf == 0)
        {
            // Did not hit a visible light
            continue;
        }

        bool isEnvMapSample = lightIndex == lightBufferParams.environmentLightParams.lightIndex;
        float targetPdf = DIGetLightSampleTargetPdf(candidateSample, surface);
        float blendedSourcePdf = ComputeLightBrdfMisWeight(surface, candidateSample, lightSourcePdf,
            isEnvMapSample ? misData.environmentMapMisWeight : misData.localLightMisWeight, 
            isEnvMapSample,
            misData.brdfMisWeight, brdfCutoff);
        float risRnd = GetNextRandom(rng);

        bool selected = StreamDIReservoirSample(state, lightIndex, randXY, risRnd, targetPdf, 1.0f / blendedSourcePdf);
        if (selected) {
            o_selectedSample = candidateSample;
        }
    }

    FinalizeDIResampling(state, 1.0, misData.numMisSamples);
    state.M = 1;

    return state;
}

// Samples emissive triangles, the environment, and BRDF-hit candidates for a given surface.
ReSTIRDIReservoir ReSTIRDISampleLightsForSurface(
    inout ReSTIRRandomSamplerState rng,
    inout ReSTIRRandomSamplerState coherentRng,
    DISurface surface,
    ReSTIRDIInitialSamplingParameters sampleParams,
    ReSTIRLightBufferParameters lightBufferParams,
    out DILightSample o_lightSample)
{
    o_lightSample = DIEmptyLightSample();

    ReSTIRDIReservoir localReservoir;
    DILightSample localSample = DIEmptyLightSample();

    DIInitialSamplingMisData misData = ComputeEffectiveInitialSamplingMisData(sampleParams, lightBufferParams);

    localReservoir = SampleLocalLights(
        rng, surface, sampleParams, misData, lightBufferParams.localLightBufferRegion, localSample);

    DILightSample environmentSample = DIEmptyLightSample();
    ReSTIRDIReservoir environmentReservoir = SampleEnvironmentLight(rng, surface,
        sampleParams, misData, lightBufferParams.environmentLightParams, environmentSample);

    DILightSample brdfSample = DIEmptyLightSample();
    ReSTIRDIReservoir brdfReservoir = SampleBrdfCandidates(rng, surface, sampleParams.numBrdfSamples, sampleParams.brdfCutoff, sampleParams.brdfRayMinT, misData, coherentRng, lightBufferParams, brdfSample);

    ReSTIRDIReservoir state = EmptyDIReservoir();
    CombineDIReservoirs(state, localReservoir, 0.5, localReservoir.targetPdf);
    bool selectEnvironment = CombineDIReservoirs(state, environmentReservoir, GetNextRandom(rng), environmentReservoir.targetPdf);
    bool selectBrdf = CombineDIReservoirs(state, brdfReservoir, GetNextRandom(rng), brdfReservoir.targetPdf);
    
    FinalizeDIResampling(state, 1.0, 1.0);
    state.M = 1;

    if (selectBrdf)
        o_lightSample = brdfSample;
    else if (selectEnvironment)
        o_lightSample = environmentSample;
    else
        o_lightSample = localSample;

	if(sampleParams.enableInitialVisibility != 0 && IsValidDIReservoir(state))
	{
		if (!DIGetConservativeVisibility(surface, o_lightSample))
        {
            StoreVisibilityInDIReservoir(state, 0, true);
        }

	}

    return state;
}

#endif
