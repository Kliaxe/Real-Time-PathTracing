#ifndef RESTIR_PT_RESERVOIR_H
#define RESTIR_PT_RESERVOIR_H

#include "ShaderIncludes/ReSTIR/PT/Debug.h.slang"

bool CombineReservoir(inout ReSTIRPTReservoir targetReservoir,
                      ReSTIRPTReservoir       candidate,
                      float                   sampleNormalization,
                      float                   sampleM,
                      float                   randomValue,
                      bool                    candidateFromHistory)
{
  // This is the RIS combine step used by the initial, temporal, and spatial
  // passes. The candidate payload changes by method, but the combination logic
  // stays the same.
  if(candidate.valid == 0 || candidate.targetPdf <= 0.0 || sampleNormalization <= 0.0 || sampleM <= 0.0)
  {
    return false;
  }

  const float risWeight = candidate.targetPdf * sampleNormalization;
  if(!(risWeight > 0.0))
  {
    return false;
  }

  targetReservoir.M += sampleM;
  const float previousWeightSum = targetReservoir.weight * targetReservoir.targetPdf * max(targetReservoir.M - sampleM, 0.0);
  const float combinedWeightSum = previousWeightSum + risWeight;
  const bool  selectCandidate   = targetReservoir.valid == 0 || randomValue * combinedWeightSum < risWeight;

  if(selectCandidate)
  {
    // Selection replaces the stored candidate payload. The final reservoir
    // weight is updated after the combined sum is known.
    targetReservoir.cachedIncidentRadiance = candidate.cachedIncidentRadiance;
    targetReservoir.localOutgoingDirection = candidate.localOutgoingDirection;
    targetReservoir.targetPdf              = candidate.targetPdf;
    targetReservoir.weight                 = 0.0;
    targetReservoir.reconnectionPoint      = candidate.reconnectionPoint;
    targetReservoir.reconnectionNormal     = candidate.reconnectionNormal;
    targetReservoir.continuationSeed       = candidate.continuationSeed;
    targetReservoir.candidateKind          = candidate.candidateKind;
    targetReservoir.reconnectionValid      = candidate.reconnectionValid;
    targetReservoir.age                    = candidateFromHistory ? (candidate.age + 1u) : 0u;
    targetReservoir.valid                  = 1u;
  }

  if(targetReservoir.valid != 0 && targetReservoir.targetPdf > 0.0 && targetReservoir.M > 0.0)
  {
    targetReservoir.weight = combinedWeightSum / (targetReservoir.targetPdf * targetReservoir.M);
  }

  return selectCandidate;
}

float3 ResolveReservoirContribution(ReSTIRPTPrimarySurface surface, ReSTIRPTReservoir reservoir, float3 cameraPosition)
{
  // Final replay uses this after the selected reservoir has been reevaluated
  // against the current receiver.
  if(!IsValidPrimarySurface(surface) || !IsValidReservoir(reservoir))
  {
    return float3(0.0);
  }

  float samplePdf       = 0.0;
  const float3 radiance = EvaluateReservoirCandidateContribution(surface, reservoir, cameraPosition, samplePdf);
  if(samplePdf <= 0.0)
  {
    return float3(0.0);
  }

  return SanitizeRadiance(radiance * reservoir.weight);
}

void ApplyReservoirBiasCorrectionScale(inout ReSTIRPTReservoir reservoir, float normalizationNumerator, float normalizationDenominator)
{
  if(!IsValidReservoir(reservoir) || !(normalizationNumerator > 0.0) || !(normalizationDenominator > 0.0))
  {
    reservoir.weight = 0.0;
    reservoir.valid  = 0u;
    return;
  }

  reservoir.weight *= (normalizationNumerator * reservoir.M) / normalizationDenominator;
}

#endif
