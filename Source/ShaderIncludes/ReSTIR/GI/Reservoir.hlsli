#ifndef RESTIR_GI_RESERVOIR_HLSLI
#define RESTIR_GI_RESERVOIR_HLSLI

#include "ReSTIR/Common/Utils/ReservoirAddressing.hlsli"

#ifndef RESTIR_GI_RESERVOIR_BUFFER
#error "RESTIR_GI_RESERVOIR_BUFFER must point to a RWStructuredBuffer<ReSTIRGIReservoir> resource"
#endif

ReSTIRGIReservoir EmptyGIReservoir()
{
  return (ReSTIRGIReservoir)0;
}

bool IsValidGIReservoir(ReSTIRGIReservoir reservoir)
{
  return reservoir.M != 0u;
}

ReSTIRGIReservoir LoadGIReservoir(ReSTIRReservoirBufferParameters reservoirParams, uint2 reservoirPosition, uint reservoirArrayIndex)
{
  const uint pointer = ReservoirPositionToPointer(reservoirParams, reservoirPosition, reservoirArrayIndex);
  return RESTIR_GI_RESERVOIR_BUFFER[pointer];
}

void StoreGIReservoir(ReSTIRGIReservoir reservoir, ReSTIRReservoirBufferParameters reservoirParams, uint2 reservoirPosition,
                      uint reservoirArrayIndex)
{
  const uint pointer = ReservoirPositionToPointer(reservoirParams, reservoirPosition, reservoirArrayIndex);
  RESTIR_GI_RESERVOIR_BUFFER[pointer] = reservoir;
}

ReSTIRGIReservoir MakeGISurfaceReservoir(float3 samplePosition, float3 sampleNormal, float3 sampleRadiance, float samplePdf)
{
  ReSTIRGIReservoir reservoir = EmptyGIReservoir();
  reservoir.samplePosition    = samplePosition;
  reservoir.sampleNormal      = sampleNormal;
  reservoir.sampleRadiance    = sampleRadiance;
  reservoir.sampleDirection   = float3(0.0);
  reservoir.sampleKind        = uint(ReSTIRGISampleKind::eReSTIRGISampleKindSurface);
  reservoir.weightSum         = samplePdf > 0.0 ? (1.0 / samplePdf) : 0.0;
  reservoir.M                 = samplePdf > 0.0 ? 1u : 0u;
  reservoir.age               = 0u;
  return reservoir;
}

ReSTIRGIReservoir MakeGIEnvironmentReservoir(float3 sampleDirection, float3 sampleRadiance, float samplePdf)
{
  ReSTIRGIReservoir reservoir = EmptyGIReservoir();
  reservoir.samplePosition    = float3(0.0);
  reservoir.sampleNormal      = float3(0.0);
  reservoir.sampleRadiance    = sampleRadiance;
  reservoir.sampleDirection   = normalize(sampleDirection);
  reservoir.sampleKind        = uint(ReSTIRGISampleKind::eReSTIRGISampleKindEnvironment);
  reservoir.weightSum         = samplePdf > 0.0 ? (1.0 / samplePdf) : 0.0;
  reservoir.M                 = samplePdf > 0.0 ? 1u : 0u;
  reservoir.age               = 0u;
  return reservoir;
}

bool CombineGIReservoirs(inout ReSTIRGIReservoir reservoir, ReSTIRGIReservoir newReservoir, float random, float targetPdf)
{
  const float risWeight = targetPdf * newReservoir.weightSum * float(newReservoir.M);
  reservoir.M += newReservoir.M;
  reservoir.weightSum += risWeight;

  const bool selectSample = (random * reservoir.weightSum <= risWeight);
  if(selectSample)
  {
    reservoir.samplePosition = newReservoir.samplePosition;
    reservoir.sampleNormal   = newReservoir.sampleNormal;
    reservoir.sampleRadiance = newReservoir.sampleRadiance;
    reservoir.age            = newReservoir.age;
    reservoir.sampleDirection = newReservoir.sampleDirection;
    reservoir.sampleKind      = newReservoir.sampleKind;
  }

  return selectSample;
}

void FinalizeGIResampling(inout ReSTIRGIReservoir reservoir, float normalizationNumerator, float normalizationDenominator)
{
  reservoir.weightSum = (normalizationDenominator == 0.0) ? 0.0 : (reservoir.weightSum * normalizationNumerator) / normalizationDenominator;
}

#endif
