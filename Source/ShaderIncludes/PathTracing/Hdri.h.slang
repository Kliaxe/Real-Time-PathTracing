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
  // Environment lookup order mirrors the runtime: procedural sky first, then HDRI, then constant background.
  if(sceneInfo.useSky != 0)
  {
    return evalSimpleSky(sceneInfo.skySimpleParam, direction);
  }

  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0)
  {
    const float2 uv = dirToLatLongUv(direction);
    return textures[sceneInfo.environmentTextureIndex].SampleLevel(uv, 0.0).xyz;
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

float ComputeEnvironmentBsdfProposalWeight(SurfaceData surface)
{
  // This ownership heuristic is intentionally narrow. The decision is made per surface, not per lobe, so using a generic
  // "smooth specular" test here would wrongly disable explicit sky lighting for glossy dielectrics whose diffuse/base
  // energy still benefits from environment NEE. Pure metals are the current case where the continuation ray is a good
  // enough stand-in for the whole surface response, because their visible energy is concentrated in the reflective lobe.
  if(!CanSampleEnvironmentReflectionDirectLight(surface))
  {
    return 0.0;
  }

  const float smoothness = 1.0 - surface.roughness;
  return clamp(surface.metallic * smoothness * smoothness, 0.0, 0.95);
}

bool ShouldBsdfOwnEnvironmentTransport(SurfaceData surface)
{
  // In the current tracer this "ownership" policy is reserved for metal-dominant surfaces. Broad lobes and glossy
  // dielectrics still keep explicit environment NEE so the sky can light their non-specular energy instead of relying
  // on rare continuation-path hits.
  return ComputeEnvironmentBsdfProposalWeight(surface) >= 0.5;
}

float EvaluateEnvironmentLightPdf(GltfSceneInfo sceneInfo, float3 receiverNormal, float3 direction)
{
  // The canonical explicit environment sampler in this tracer is the scene-wide HDRI proposal only.
  return EvaluateEnvironmentBaseLightPdf(sceneInfo, receiverNormal, direction);
}

bool SampleEnvironmentBaseLight(GltfSceneInfo sceneInfo,
                                float3 receiverNormal,
                                inout uint seed,
                                out float3 lightDir,
                                out float3 radiance,
                                out float lightPdf)
{
  radiance = float3(0.0);
  lightPdf = 0.0;

  if(sceneInfo.useHdrEnv != 0 && sceneInfo.environmentTextureIndex >= 0 && sceneInfo.environmentWidth > 0
     && sceneInfo.environmentHeight > 0)
  {
    // The CPU builds a flat texel CDF, so shader sampling is just "pick texel from CDF, then jitter inside the texel".
    const uint  texelCount = sceneInfo.environmentWidth * sceneInfo.environmentHeight;
    const uint  index      = BinarySearchCdf(sceneInfo.environmentCdf, texelCount, NextRandom(seed));
    const uint  x          = index % sceneInfo.environmentWidth;
    const uint  y          = index / sceneInfo.environmentWidth;
    const float2 uv        = (float2(float(x) + NextRandom(seed), float(y) + NextRandom(seed)))
                      / float2(float(sceneInfo.environmentWidth), float(sceneInfo.environmentHeight));

    lightDir  = LatLongUvToDir(uv);
    radiance  = SampleEnvironment(sceneInfo, lightDir);
    lightPdf  = EvaluateEnvironmentBaseLightPdf(sceneInfo, receiverNormal, lightDir);
    return lightPdf > 0.0 && SafeMax3(radiance) > 0.0;
  }

  // The fallback path keeps direct-environment lighting alive even when no HDRI importance data exists.
  lightDir = SampleCosineHemisphereDirection(receiverNormal, seed, lightPdf);
  radiance = SampleEnvironment(sceneInfo, lightDir);
  return lightPdf > 0.0 && SafeMax3(radiance) > 0.0;
}

#endif
