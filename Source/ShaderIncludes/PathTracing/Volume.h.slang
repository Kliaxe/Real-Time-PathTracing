#ifndef PATH_TRACING_VOLUME_H
#define PATH_TRACING_VOLUME_H

#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"

// -------------------------------------------------------------------------
//    Volume attenuation
// -------------------------------------------------------------------------

float3 BuildAttenuationCoefficient(float3 attenuationColor, float attenuationDistance)
{
  // Converts glTF's "color at distance" volume representation into an exponential extinction coefficient.
  if(attenuationDistance <= 0.0)
  {
    return float3(0.0);
  }

  const float3 safeColor = max(attenuationColor, float3(1.0e-3));
  return -log(safeColor) / attenuationDistance;
}

float3 EvaluateMediumTransmittance(float3 attenuationCoefficient, float distance)
{
  // Standard Beer-Lambert transmittance for the current medium segment.
  return exp(-attenuationCoefficient * max(distance, 0.0));
}

void ApplyCurrentMediumAttenuation(inout PathPayload payload, float segmentDistance)
{
  // Applies absorption to the segment that just finished tracing before the next surface event is shaded.
  if(payload.mediumActive == 0)
  {
    return;
  }

  // Thin-volume authoring can produce unrealistically tiny geometric path lengths, so clamp to the authored thickness floor.
  const float effectiveDistance = max(segmentDistance, payload.mediumThickness);
  payload.throughput *= EvaluateMediumTransmittance(payload.mediumAttenuation, effectiveDistance);
}

bool HasVolumeAttenuation(SurfaceData surface)
{
  // White attenuationColor means "no absorption", so require an actual tint before enabling volume attenuation.
  return surface.transmission > 0.0 && surface.volumeThickness > 0.0 && surface.attenuationDistance > 0.0
         && SafeMax3(abs(float3(1.0) - surface.attenuationColor)) > 1.0e-4;
}

#endif
