#ifndef PATH_TRACING_HDRI_H
#define PATH_TRACING_HDRI_H

#include "nvshaders/sky_functions.h.slang"
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"

// -------------------------------------------------------------------------
//    Environment lookup
// -------------------------------------------------------------------------

float2 dirToLatLongUv(float3 dir)
{
  // Converts a world-space direction into the texture-space convention used by the lat-long HDRIs in this project.
  const float kInvTwoPi = 0.15915494309;

  const float3 n = normalize(dir);
  const float  u = 0.5 - atan2(n.z, n.x) * kInvTwoPi;
  const float  v = 0.5 - asin(clamp(n.y, -1.0, 1.0)) * kInvPi;
  return float2(u, v);
}

float3 LatLongUvToDir(float2 uv)
{
  // Inverse of dirToLatLongUv(), used by the environment importance sampler.
  const float theta = 3.14159265359 * uv.y;
  const float phi   = 6.28318530718 * (0.5 - uv.x);
  const float sinTheta = sin(theta);
  return float3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
}

float3 SampleEnvironment(GltfSceneInfo sceneInfo, float3 direction)
{
  // HDRI has priority because the environment PDF below is built from the HDRI importance table.
  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0)
  {
    const float2 uv = dirToLatLongUv(direction);
    return textures[sceneInfo.environmentTextureIndex].SampleLevel(uv, 0.0).xyz;
  }

  if(sceneInfo.useSky != 0)
  {
    return evalSimpleSky(sceneInfo.skySimpleParam, direction);
  }

  return sceneInfo.backgroundColor;
}

float EvaluateEnvironmentBaseLightPdf(GltfSceneInfo sceneInfo, float3 receiverNormal, float3 direction)
{
  // Baseline environment proposal: HDRI luminance CDF when available, otherwise cosine-weighted fallback.
  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0 && sceneInfo.environmentWidth > 0
     && sceneInfo.environmentHeight > 0)
  {
    const float2 uv = dirToLatLongUv(direction);
    const uint   x  = min(uint(uv.x * float(sceneInfo.environmentWidth)), sceneInfo.environmentWidth - 1);
    const uint   y  = min(uint(uv.y * float(sceneInfo.environmentHeight)), sceneInfo.environmentHeight - 1);
    const uint   index = y * sceneInfo.environmentWidth + x;
    const float  texelPdf = sceneInfo.environmentPdf[index];
    const float  sinTheta = max(sin(3.14159265359 * uv.y), 1.0e-6);
    return texelPdf * float(sceneInfo.environmentWidth * sceneInfo.environmentHeight) / (19.7392088022 * sinTheta);
  }

  // Without an HDRI importance table, direct environment lighting falls back to cosine-weighted sampling around the receiver.
  return PdfCosineHemisphere(receiverNormal, direction);
}

bool CanSampleEnvironmentReflectionDirectLight(SurfaceData surface)
{
  // Opaque surfaces sample the environment explicitly.
  // Transmissive materials currently own environment transport through continuation rays only, because the explicit
  // environment MIS contract for their reflection/transmission events has not been derived and validated yet.
  return surface.transmission <= 0.001;
}

bool CanSampleEnvironmentTransmissionExitLight(SurfaceData surface)
{
  // Transmission currently owns environment transport through continuation rays only.
  // That keeps the glass path on one estimator until the transmission-side environment MIS contract is derived and verified.
  return false;
}

float EvaluateEnvironmentLightPdf(GltfSceneInfo sceneInfo, float3 receiverNormal, float3 direction)
{
  // The canonical explicit environment sampler in this tracer is the scene-wide HDRI proposal only.
  return EvaluateEnvironmentBaseLightPdf(sceneInfo, receiverNormal, direction);
}

float2 ExpandEnvironmentJitterFromScalar(float sampleValue)
{
  // DI reservoirs expose two uniform samples. HDRI texel selection needs one
  // sample for the CDF and a 2D jitter within the chosen texel, so expand the
  // second scalar into a stable decorrelated pair.
  const float2 jitter = frac(float2(sampleValue * 0.7548776662466927 + 0.5, sampleValue * 0.5698402909980532 + 0.25));
  return jitter;
}

bool SampleEnvironmentBaseLightFromRandoms(GltfSceneInfo sceneInfo,
                                           float3 receiverNormal,
                                           float sampleSelector,
                                           float2 sampleJitter,
                                           out float3 lightDir,
                                           out float3 radiance,
                                           out float lightPdf)
{
  radiance = float3(0.0);
  lightPdf = 0.0;

  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0 && sceneInfo.environmentWidth > 0
     && sceneInfo.environmentHeight > 0)
  {
    const uint   texelCount = sceneInfo.environmentWidth * sceneInfo.environmentHeight;
    const uint   index      = BinarySearchCdf(sceneInfo.environmentCdf, texelCount, sampleSelector);
    const uint   x          = index % sceneInfo.environmentWidth;
    const uint   y          = index / sceneInfo.environmentWidth;
    const float2 uv         = (float2(float(x), float(y)) + frac(sampleJitter))
                      / float2(float(sceneInfo.environmentWidth), float(sceneInfo.environmentHeight));

    lightDir  = LatLongUvToDir(uv);
    radiance  = SampleEnvironment(sceneInfo, lightDir);
    lightPdf  = EvaluateEnvironmentBaseLightPdf(sceneInfo, receiverNormal, lightDir);
    return lightPdf > 0.0 && SafeMax3(radiance) > 0.0;
  }

  lightDir = SampleCosineHemisphereDirection(receiverNormal, frac(sampleJitter), lightPdf);
  radiance = SampleEnvironment(sceneInfo, lightDir);
  return lightPdf > 0.0 && SafeMax3(radiance) > 0.0;
}

bool SampleEnvironmentBaseLight(GltfSceneInfo sceneInfo,
                                float3 receiverNormal,
                                inout uint seed,
                                out float3 lightDir,
                                out float3 radiance,
                                out float lightPdf)
{
  return SampleEnvironmentBaseLightFromRandoms(sceneInfo, receiverNormal, NextRandom(seed), float2(NextRandom(seed), NextRandom(seed)),
                                               lightDir, radiance, lightPdf);
}

#endif
