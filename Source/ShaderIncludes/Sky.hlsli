#ifndef RTPT_SHADER_SKY_HLSLI
#define RTPT_SHADER_SKY_HLSLI

#include "Common/SkyIo.h"

// Procedural sky radiance for a world-space direction: a sky/horizon/ground gradient plus a glow around the sun.
float3 EvaluateSimpleSky(SkySimpleParameters parameters, float3 direction)
{
  // Gradient
  // Elevation is the angle above the horizon in radians. horizonSize is the angular band over which the horizon color blends into the sky above and the ground below.

  const float3 skyColor     = parameters.skyColor * parameters.brightness;
  const float3 horizonColor = parameters.horizonColor * parameters.brightness;
  const float3 groundColor  = parameters.groundColor * parameters.brightness;

  const float elevation    = asin(clamp(dot(direction, parameters.directionUp), -1.0f, 1.0f));
  const float top          = smoothstep(0.0f, parameters.horizonSize, elevation);
  const float bottom       = smoothstep(0.0f, parameters.horizonSize, -elevation);
  const float3 environment = lerp(lerp(horizonColor, groundColor, bottom), skyColor, top);

  // Sun glow
  // The glow is at full strength inside the sun's half angle and falls off over glowSize on either side of that edge. glowSharpness then shapes the falloff.
  // The dot is clamped at zero, so everything past 90 degrees from the sun counts as 90 degrees, which is already outside any glow.
  // The glow's radiance is lightRadiance tinted by sunColor and scaled by sunIntensity; both default to 1, which leaves lightRadiance alone.

  const float angleToLight    = acos(clamp(dot(direction, parameters.sunDirection), 0.0f, 1.0f));
  const float halfAngularSize = parameters.angularSizeOfLight * 0.5f;
  const float glowInput       = clamp(2.0f * (1.0f - smoothstep(halfAngularSize - parameters.glowSize, halfAngularSize + parameters.glowSize, angleToLight)), 0.0f, 1.0f);
  const float glowIntensity   = parameters.glowIntensity * pow(glowInput, parameters.glowSharpness);
  const float3 sunRadiance    = parameters.lightRadiance * parameters.sunColor * parameters.sunIntensity;

  return environment + glowIntensity * sunRadiance;
}

#endif  // RTPT_SHADER_SKY_HLSLI
