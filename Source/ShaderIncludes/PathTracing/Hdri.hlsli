#ifndef PATH_TRACING_HDRI_H
#define PATH_TRACING_HDRI_H

#include "ShaderIncludes/Sky.hlsli"
#include "ShaderIncludes/PathTracing/Common.hlsli"
#include "ShaderIncludes/PathTracing/Utility.hlsli"
#include "ShaderIncludes/PathTracing/MonteCarlo.hlsli"

// Environment lookup
// Radiance, PDF, and importance sampling for the environment: an HDRI when one is loaded, otherwise the procedural sky or a flat background color.

// Converts a world-space direction into the texture-space convention used by the lat-long HDRIs in this project.
// V runs from +Y at the top row to -Y at the bottom.
float2 dirToLatLongUv(float3 dir)
{
  const float kInvTwoPi = 0.15915494309;

  const float3 n = normalize(dir);
  const float  u = 0.5 - atan2(n.z, n.x) * kInvTwoPi;
  const float  v = 0.5 - asin(clamp(n.y, -1.0, 1.0)) * kInvPi;

  return float2(u, v);
}

// Inverse of dirToLatLongUv(), used by the environment importance sampler. Theta is measured from +Y.
float3 LatLongUvToDir(float2 uv)
{
  const float theta    = 3.14159265359 * uv.y;
  const float phi      = 6.28318530718 * (0.5 - uv.x);
  const float sinTheta = sin(theta);

  return float3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
}

// Environment radiance seen along a direction.
float3 SampleEnvironment(GltfSceneInfo sceneInfo, float3 direction)
{
  // HDRI has priority because the environment PDF below is built from the HDRI importance table.
  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0)
  {
    const float2 uv = dirToLatLongUv(direction);

    return SampleSceneTextureLevel(sceneInfo.environmentTextureIndex, uv, 0.0).xyz;
  }

  if(sceneInfo.useSky != 0)
  {
    return EvaluateSimpleSky(sceneInfo.skySimpleParam, direction);
  }

  return sceneInfo.backgroundColor;
}

// Solid-angle PDF of the baseline environment proposal: the HDRI luminance CDF when available, otherwise a cosine-weighted fallback.
float EvaluateEnvironmentBaseLightPdf(GltfSceneInfo sceneInfo, float3 receiverNormal, float3 direction)
{
  // HDRI texel PDF
  // The CPU table stores each texel's discrete probability, weighted by luminance times sin(theta). Multiplying by the texel count gives a density over the unit UV square.
  // The lat-long mapping has dOmega = 2 * pi^2 * sin(theta) du dv, so dividing by 19.7392088022 (2 * pi^2) and sin(theta) converts that density to solid angle. The sin(theta) floor avoids dividing by zero at the poles.

  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0 && sceneInfo.environmentWidth > 0 && sceneInfo.environmentHeight > 0)
  {
    const float2 uv       = dirToLatLongUv(direction);
    const uint   x        = min(uint(uv.x * float(sceneInfo.environmentWidth)), sceneInfo.environmentWidth - 1);
    const uint   y        = min(uint(uv.y * float(sceneInfo.environmentHeight)), sceneInfo.environmentHeight - 1);
    const uint   index    = y * sceneInfo.environmentWidth + x;
    const float  texelPdf = vk::RawBufferLoad<float>(sceneInfo.environmentPdf + uint64_t(index) * 4u, 4);
    const float  sinTheta = max(sin(3.14159265359 * uv.y), 1.0e-6);

    return texelPdf * float(sceneInfo.environmentWidth * sceneInfo.environmentHeight) / (19.7392088022 * sinTheta);
  }

  // Without an HDRI importance table, direct environment lighting falls back to cosine-weighted sampling around the receiver.
  return PdfCosineHemisphere(receiverNormal, direction);
}

// Whether a hit on this surface draws an explicit environment sample for its reflection side.
// Opaque surfaces sample the environment explicitly. Transmissive materials currently own environment transport through continuation rays only, because the explicit environment MIS contract for their reflection/transmission events has not been derived and validated yet.
bool CanSampleEnvironmentReflectionDirectLight(SurfaceData surface)
{
  return surface.transmission <= 0.001;
}

// Whether a transmission event draws an explicit environment sample on the exit side. Always false for now.
// Transmission currently owns environment transport through continuation rays only. That keeps the glass path on one estimator until the transmission-side environment MIS contract is derived and verified.
bool CanSampleEnvironmentTransmissionExitLight(SurfaceData surface)
{
  return false;
}

// The canonical explicit environment sampler in this tracer is the scene-wide HDRI proposal only.
float EvaluateEnvironmentLightPdf(GltfSceneInfo sceneInfo, float3 receiverNormal, float3 direction)
{
  return EvaluateEnvironmentBaseLightPdf(sceneInfo, receiverNormal, direction);
}

// Expands one uniform scalar into a 2D in-texel jitter.
// The caller has one uniform sample left to spend, but HDRI texel selection needs a CDF probe plus a 2D jitter within the chosen texel. Expanding the remaining scalar into a stable pair, rather than drawing again, keeps the sampling stream reproducible for replay.
// The multipliers are the R2 sequence's increments (the inverse plastic number and its square). The pair is a deterministic function of one value, so the two coordinates are not independent.
float2 ExpandEnvironmentJitterFromScalar(float sampleValue)
{
  const float2 jitter = frac(float2(sampleValue * 0.7548776662466927 + 0.5, sampleValue * 0.5698402909980532 + 0.25));

  return jitter;
}

// Draws an environment light direction from explicit random numbers, returning its radiance and solid-angle PDF.
// The PDF comes from EvaluateEnvironmentBaseLightPdf so sampling and MIS evaluation cannot disagree.
bool SampleEnvironmentBaseLightFromRandoms(GltfSceneInfo sceneInfo, float3 receiverNormal, float sampleSelector, float2 sampleJitter, out float3 lightDir, out float3 radiance, out float lightPdf)
{
  radiance = (float3)0.0;
  lightPdf = 0.0;

  // HDRI importance sampling
  // The selector picks a texel through the luminance CDF, and the jitter places the direction uniformly in UV inside it.

  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0 && sceneInfo.environmentWidth > 0 && sceneInfo.environmentHeight > 0)
  {
    const uint   texelCount = sceneInfo.environmentWidth * sceneInfo.environmentHeight;
    const uint   index      = BinarySearchCdf(sceneInfo.environmentCdf, texelCount, sampleSelector);
    const uint   x          = index % sceneInfo.environmentWidth;
    const uint   y          = index / sceneInfo.environmentWidth;
    const float2 uv         = (float2(float(x), float(y)) + frac(sampleJitter)) / float2(float(sceneInfo.environmentWidth), float(sceneInfo.environmentHeight));

    lightDir  = LatLongUvToDir(uv);
    radiance  = SampleEnvironment(sceneInfo, lightDir);
    lightPdf  = EvaluateEnvironmentBaseLightPdf(sceneInfo, receiverNormal, lightDir);

    return lightPdf > 0.0 && SafeMax3(radiance) > 0.0;
  }

  // Cosine fallback
  // Without an importance table the environment is sampled around the receiver normal; the selector goes unused.

  lightDir = SampleCosineHemisphereDirection(receiverNormal, frac(sampleJitter), lightPdf);
  radiance = SampleEnvironment(sceneInfo, lightDir);

  return lightPdf > 0.0 && SafeMax3(radiance) > 0.0;
}

// Draws an environment light direction from the path's RNG stream: one selector and a 2D jitter.
bool SampleEnvironmentBaseLight(GltfSceneInfo sceneInfo, float3 receiverNormal, inout PathSampleStream seed, out float3 lightDir, out float3 radiance, out float lightPdf)
{
  const float selector = NextRandom(seed);
  const float2 jitter  = NextRandom2(seed);

  return SampleEnvironmentBaseLightFromRandoms(sceneInfo, receiverNormal, selector, jitter, lightDir, radiance, lightPdf);
}

#endif

