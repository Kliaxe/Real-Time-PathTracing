#ifndef RESTIR_GI_SPATIAL_RESAMPLING_HLSLI
#define RESTIR_GI_SPATIAL_RESAMPLING_HLSLI

#ifndef RESTIR_GI_ALLOWED_BIAS_CORRECTION
#define RESTIR_GI_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_RAY_TRACED
#endif

#include "ReSTIR/Common/Utils/Math.hlsli"
#include "ReSTIR/Common/Utils/Checkerboard.hlsli"
#include "ReSTIR/Common/Utils/ReservoirAddressing.hlsli"

int2 CalculateGISpatialResamplingOffset(int sampleIndex, float radius, uint neighborOffsetMask)
{
  sampleIndex &= int(neighborOffsetMask);
  return int2(float2(giNeighborOffsetBuffer[sampleIndex].xy) * radius);
}

ReSTIRGIReservoir RunGISpatialResampling(uint2 pixelPosition,
                                         GISurface surface,
                                         uint sourceBufferIndex,
                                         ReSTIRGIReservoir inputReservoir,
                                         inout ReSTIRRandomSamplerState rng,
                                         ReSTIRRuntimeParameters runtimeParams,
                                         ReSTIRReservoirBufferParameters reservoirParams,
                                         ReSTIRGISpatialResamplingParameters spatialParams)
{
  const uint numSamples = spatialParams.numSamples;

  ReSTIRGIReservoir currentReservoir = EmptyGIReservoir();
  float selectedTargetPdf            = 0.0;
  if(IsValidGIReservoir(inputReservoir))
  {
    selectedTargetPdf = GIGetSampleTargetPdfForSurface(inputReservoir, surface);
    CombineGIReservoirs(currentReservoir, inputReservoir, 0.5, selectedTargetPdf);
  }

  uint cachedResult = 0u;
  int selectedNeighbor = -1;
  const int neighborSampleStart = int(GetNextRandom(rng) * float(runtimeParams.neighborOffsetMask));

  for(uint i = 0; i < numSamples; ++i)
  {
    int2 neighborPixel = int2(pixelPosition) + CalculateGISpatialResamplingOffset(neighborSampleStart + int(i), spatialParams.samplingRadius,
                                                                                  runtimeParams.neighborOffsetMask);
    neighborPixel = GIClampSamplePositionIntoView(neighborPixel, false);
    ActivateCheckerboardPixel(neighborPixel, false, runtimeParams.activeCheckerboardField);

    const GISurface neighborSurface = GILoadGBufferSurface(neighborPixel, false);
    if(!GIIsSurfaceValid(neighborSurface))
    {
      continue;
    }

    if(!IsValidNeighbor(GIGetSurfaceNormal(surface), GIGetSurfaceNormal(neighborSurface), GIGetSurfaceLinearDepth(surface),
                        GIGetSurfaceLinearDepth(neighborSurface), spatialParams.normalThreshold, spatialParams.depthThreshold))
    {
      continue;
    }

    if(!GIAreMaterialsSimilar(GIGetMaterial(surface), GIGetMaterial(neighborSurface)))
    {
      continue;
    }

    const uint2 reservoirPosition = PixelPosToReservoirPos(uint2(neighborPixel), runtimeParams.activeCheckerboardField);
    ReSTIRGIReservoir neighborReservoir = LoadGIReservoir(reservoirParams, reservoirPosition, sourceBufferIndex);
    if(!IsValidGIReservoir(neighborReservoir))
    {
      continue;
    }

    float jacobian = GICalculateJacobianForSurface(surface, neighborSurface, neighborReservoir);
    const float targetPdf = GIGetSampleTargetPdfForSurface(neighborReservoir, surface);
    if(!GIValidateSampleWithJacobian(jacobian))
    {
      continue;
    }

    cachedResult |= (1u << i);
    if(CombineGIReservoirs(currentReservoir, neighborReservoir, GetNextRandom(rng), targetPdf * jacobian))
    {
      selectedNeighbor = int(i);
      selectedTargetPdf = targetPdf;
    }
  }

  if(spatialParams.biasCorrectionMode >= RESTIR_BIAS_CORRECTION_BASIC)
  {
    float pi    = selectedTargetPdf;
    float piSum = selectedTargetPdf * float(inputReservoir.M);

    for(uint i = 0; i < numSamples; ++i)
    {
      if((cachedResult & (1u << i)) == 0u)
      {
        continue;
      }

      int2 neighborPixel = int2(pixelPosition) + CalculateGISpatialResamplingOffset(neighborSampleStart + int(i), spatialParams.samplingRadius,
                                                                                    runtimeParams.neighborOffsetMask);
      neighborPixel = GIClampSamplePositionIntoView(neighborPixel, false);
      ActivateCheckerboardPixel(neighborPixel, false, runtimeParams.activeCheckerboardField);

      const GISurface neighborSurface = GILoadGBufferSurface(neighborPixel, false);
      const uint2 reservoirPosition   = PixelPosToReservoirPos(uint2(neighborPixel), runtimeParams.activeCheckerboardField);
      const ReSTIRGIReservoir neighborReservoir = LoadGIReservoir(reservoirParams, reservoirPosition, sourceBufferIndex);

      float ps = GIGetSampleTargetPdfForSurface(currentReservoir, neighborSurface);
#if RESTIR_GI_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_RAY_TRACED
      if(spatialParams.biasCorrectionMode == RESTIR_BIAS_CORRECTION_RAY_TRACED && ps > 0.0
         && !GIGetConservativeVisibility(neighborSurface, currentReservoir))
      {
        ps = 0.0;
      }
#endif

      pi = (selectedNeighbor == int(i)) ? ps : pi;
      piSum += ps * float(neighborReservoir.M);
    }

    FinalizeGIResampling(currentReservoir, pi, selectedTargetPdf * piSum);
  }
  else
  {
    FinalizeGIResampling(currentReservoir, 1.0, float(currentReservoir.M) * selectedTargetPdf);
  }

  return currentReservoir;
}

#endif
