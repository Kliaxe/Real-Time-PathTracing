#ifndef PATH_TRACING_VOLUME_H
#define PATH_TRACING_VOLUME_H

#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"

// Volume attenuation
// Absorption inside transmissive glTF volumes (KHR_materials_volume). The path payload tracks the medium it is currently inside, and each segment through that medium is attenuated by Beer-Lambert transmittance.

// Converts glTF's "color at distance" volume representation into an exponential extinction coefficient.
// The result is chosen so that transmittance over attenuationDistance equals attenuationColor.
float3 BuildAttenuationCoefficient(float3 attenuationColor, float attenuationDistance)
{
  if(attenuationDistance <= 0.0)
  {
    return (float3)0.0;
  }

  // The floor keeps log() finite for black channels.
  const float3 safeColor = max(attenuationColor, (float3)1.0e-3);

  return -log(safeColor) / attenuationDistance;
}

// Standard Beer-Lambert transmittance for the current medium segment.
float3 EvaluateMediumTransmittance(float3 attenuationCoefficient, float distance)
{
  return exp(-attenuationCoefficient * max(distance, 0.0));
}

// Applies absorption to the segment that just finished tracing before the next surface event is shaded.
void ApplyCurrentMediumAttenuation(inout PathPayload payload, float segmentDistance)
{
  if(payload.mediumActive == 0)
  {
    return;
  }

  // Beer-Lambert attenuation depends on the actual distance traveled in the medium along this ray segment.
  payload.throughput *= EvaluateMediumTransmittance(payload.mediumAttenuation, segmentDistance);
}

// White attenuationColor means "no absorption", so require an actual tint before enabling volume attenuation.
bool HasVolumeAttenuation(SurfaceData surface)
{
  return surface.transmission > 0.0 && surface.volumeThickness > 0.0 && surface.attenuationDistance > 0.0 && SafeMax3(abs((float3)1.0 - surface.attenuationColor)) > 1.0e-4;
}

#endif

