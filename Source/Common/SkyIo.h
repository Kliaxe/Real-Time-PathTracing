#ifndef RTPT_SKY_IO_H
#define RTPT_SKY_IO_H

#include "Common/ShaderTypes.h"

NAMESPACE_SHADERIO_BEGIN()

// Descriptor bindings of the procedural sky compute pass.
enum SkyBindings
{
  eSkyOutImage = 0,
};

// SkySamplingResult
// A sampled sky direction with its probability density and radiance.

struct SkySamplingResult
{
  // Sampled world-space direction.
  float3 direction;

  // Probability density of the sampled direction.
  float pdf;

  // Sky radiance arriving from the sampled direction.
  float3 radiance;
};

// SkySimpleParameters
// Parameters for the procedural sky shared by scene upload and shader code. Stored inline in GltfSceneInfo.
// Each vec3 is paired with a scalar so the struct packs into 16-byte rows.

struct SkySimpleParameters
{
  // World-space unit direction toward the sun.
  float3 sunDirection RTPT_DEFAULT(float3(-1.23413404e-08F, 0.707106829F, 0.707106709F));

  // Full angular diameter of the sun in radians; the glow is at full strength inside half of it.
  float angularSizeOfLight RTPT_DEFAULT(0.059F);

  // Tint multiplied into the sun glow together with lightRadiance and sunIntensity. White leaves the glow unchanged, and the sky gradient never sees it.
  float3 sunColor RTPT_DEFAULT(float3(1.0F, 1.0F, 1.0F));

  // Angular width in radians over which the sun glow fades, on each side of the sun's edge.
  float glowSize RTPT_DEFAULT(0.091F);

  // Color of the sky above the horizon band, before brightness is applied.
  float3 skyColor RTPT_DEFAULT(float3(0.17F, 0.37F, 0.65F));

  // Peak strength of the sun glow.
  float glowIntensity RTPT_DEFAULT(0.9F);

  // Color at the horizon, before brightness is applied.
  float3 horizonColor RTPT_DEFAULT(float3(0.50F, 0.70F, 0.92F));

  // Elevation band in radians over which the horizon color blends into the sky and the ground.
  float horizonSize RTPT_DEFAULT(0.5F);

  // Color below the horizon band, before brightness is applied.
  float3 groundColor RTPT_DEFAULT(float3(0.62F, 0.59F, 0.55F));

  // Exponent shaping the sun glow falloff.
  float glowSharpness RTPT_DEFAULT(4.F);

  // World-space up direction that elevation is measured against.
  float3 directionUp RTPT_DEFAULT(float3(0.F, 1.F, 0.F));

  // Scalar multiplier on the sun glow, alongside sunColor. Unlike brightness it leaves the sky gradient alone, so the sun can be dimmed or boosted on its own.
  float sunIntensity RTPT_DEFAULT(1.0F);

  // Base radiance of the sun glow, before sunColor and sunIntensity are applied.
  float3 lightRadiance RTPT_DEFAULT(float3(1.0F, 1.0F, 1.0F));

  // Multiplier on the sky, horizon, and ground colors.
  float brightness RTPT_DEFAULT(1.0F);
};

NAMESPACE_SHADERIO_END()

// Also defines the older guard name. No file in this repository tests it.
#ifndef SKY_SHADERIO_H
#define SKY_SHADERIO_H 1
#endif

#endif  // RTPT_SKY_IO_H
