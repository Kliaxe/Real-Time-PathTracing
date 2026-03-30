#ifndef RESTIR_GI_TEMPORAL_RESAMPLING_HLSLI
#define RESTIR_GI_TEMPORAL_RESAMPLING_HLSLI

#ifndef RESTIR_GI_ALLOWED_BIAS_CORRECTION
#define RESTIR_GI_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_RAY_TRACED
#endif

#include "ReSTIR/Common/Utils/Checkerboard.hlsli"
#include "ReSTIR/Common/Utils/Math.hlsli"
#include "ReSTIR/Common/Utils/ReservoirAddressing.hlsli"

int2 CalculateGITemporalResamplingOffset(int sampleIndex, int radius)
{
  sampleIndex &= 7;

  const int mask2 = (sampleIndex >> 1) & 0x01;
  const int mask4 = 1 - ((sampleIndex >> 2) & 0x01);
  const int tmp0  = -1 + 2 * (sampleIndex & 0x01);
  const int tmp1  = 1 - 2 * mask2;
  const int tmp2  = mask4 | mask2;
  const int tmp3  = mask4 | (1 - mask2);

  return int2(tmp0, tmp0 * tmp1) * int2(tmp2, tmp3) * radius;
}

ReSTIRGIReservoir RunGITemporalResampling(uint2 pixelPosition,
                                          GISurface surface,
                                          float3 screenSpaceMotion,
                                          uint sourceBufferIndex,
                                          ReSTIRGIReservoir inputReservoir,
                                          inout ReSTIRRandomSamplerState rng,
                                          ReSTIRRuntimeParameters runtimeParams,
                                          ReSTIRReservoirBufferParameters reservoirParams,
                                          ReSTIRGITemporalResamplingParameters temporalParams)
{
  int2 prevPos = int2(round(float2(pixelPosition) + screenSpaceMotion.xy));
  const float expectedPrevLinearDepth = GIGetSurfaceLinearDepth(surface) + screenSpaceMotion.z;
  const int radius = (runtimeParams.activeCheckerboardField == 0) ? 1 : 2;

  ReSTIRGIReservoir temporalReservoir = EmptyGIReservoir();
  bool foundTemporalReservoir         = false;
  GISurface temporalSurface           = EmptyGIPrimarySurface();

  const int temporalSampleStart = int(GetNextRandom(rng) * 8.0);
  const int temporalSampleCount = 5;
  const int sampleCount = temporalSampleCount + (temporalParams.enableFallbackSampling != 0u ? 1 : 0);

  for(int i = 0; i < sampleCount; ++i)
  {
    const bool isFirstSample    = (i == 0);
    const bool isFallbackSample = (i == temporalSampleCount);

    int2 offset = int2(0, 0);
    if(isFallbackSample)
    {
      prevPos = int2(pixelPosition);
    }
    else if(!isFirstSample)
    {
      offset = CalculateGITemporalResamplingOffset(temporalSampleStart + i, radius);
    }

    int2 candidatePixel = prevPos + offset;
    if((temporalParams.enablePermutationSampling != 0u && isFirstSample) || isFallbackSample)
    {
      ApplyPermutationSampling(candidatePixel, temporalParams.uniformRandomNumber);
    }

    ActivateCheckerboardPixel(candidatePixel, true, runtimeParams.activeCheckerboardField);
    temporalSurface = GILoadGBufferSurface(candidatePixel, true);
    if(!GIIsSurfaceValid(temporalSurface))
    {
      continue;
    }

    if(!isFallbackSample && !IsValidNeighbor(GIGetSurfaceNormal(surface), GIGetSurfaceNormal(temporalSurface), expectedPrevLinearDepth,
                                             GIGetSurfaceLinearDepth(temporalSurface), temporalParams.normalThreshold,
                                             temporalParams.depthThreshold))
    {
      continue;
    }

    if(!GIAreMaterialsSimilar(GIGetMaterial(surface), GIGetMaterial(temporalSurface)))
    {
      continue;
    }

    const uint2 prevReservoirPos = PixelPosToReservoirPos(uint2(candidatePixel), runtimeParams.activeCheckerboardField);
    temporalReservoir            = LoadGIReservoir(reservoirParams, prevReservoirPos, sourceBufferIndex);
    if(!IsValidGIReservoir(temporalReservoir))
    {
      continue;
    }

    foundTemporalReservoir = true;
    break;
  }

  ReSTIRGIReservoir currentReservoir = EmptyGIReservoir();

  float selectedTargetPdf = 0.0;
  if(IsValidGIReservoir(inputReservoir))
  {
    selectedTargetPdf = GIGetSampleTargetPdfForSurface(inputReservoir, surface);
    CombineGIReservoirs(currentReservoir, inputReservoir, 0.5, selectedTargetPdf);
  }

  if(foundTemporalReservoir)
  {
    float jacobian = GICalculateJacobianForSurface(surface, temporalSurface, temporalReservoir);
    if(!GIValidateSampleWithJacobian(jacobian))
    {
      foundTemporalReservoir = false;
    }
    else
    {
      temporalReservoir.weightSum *= jacobian;
      temporalReservoir.M          = min(temporalReservoir.M, temporalParams.maxHistoryLength);
      temporalReservoir.age += 1u;
      if(temporalReservoir.age > temporalParams.maxReservoirAge)
      {
        foundTemporalReservoir = false;
      }
    }
  }

  bool selectedPreviousSample = false;
  if(foundTemporalReservoir)
  {
    const float targetPdf = GIGetSampleTargetPdfForSurface(temporalReservoir, surface);
    selectedPreviousSample = CombineGIReservoirs(currentReservoir, temporalReservoir, GetNextRandom(rng), targetPdf);
    if(selectedPreviousSample)
    {
      selectedTargetPdf = targetPdf;
    }
  }

  if(temporalParams.biasCorrectionMode >= RESTIR_BIAS_CORRECTION_BASIC)
  {
    float pi    = selectedTargetPdf;
    float piSum = selectedTargetPdf * float(inputReservoir.M);

    if(IsValidGIReservoir(currentReservoir) && foundTemporalReservoir)
    {
      float temporalP = GIGetSampleTargetPdfForSurface(currentReservoir, temporalSurface);
#if RESTIR_GI_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_RAY_TRACED
      if(temporalParams.biasCorrectionMode == RESTIR_BIAS_CORRECTION_RAY_TRACED && temporalP > 0.0
         && !GIGetTemporalConservativeVisibility(surface, temporalSurface, currentReservoir))
      {
        temporalP = 0.0;
      }
#endif

      pi = selectedPreviousSample ? temporalP : pi;
      piSum += temporalP * float(temporalReservoir.M);
    }

    FinalizeGIResampling(currentReservoir, pi, piSum * selectedTargetPdf);
  }
  else
  {
    FinalizeGIResampling(currentReservoir, 1.0, selectedTargetPdf * float(currentReservoir.M));
  }

  return currentReservoir;
}

#endif
