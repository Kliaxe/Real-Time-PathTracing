#ifndef PATH_TRACING_MONTE_CARLO_H
#define PATH_TRACING_MONTE_CARLO_H

#include "nvshaders/random.h.slang"
#include "nvshaders/functions.h.slang"
#include "ShaderIncludes/PathTracing/Common.h.slang"

// -------------------------------------------------------------------------
//    Random numbers and importance sampling
// -------------------------------------------------------------------------

float NextRandom(inout uint seed)
{
  // Central wrapper so the rest of the tracer does not depend directly on the underlying RNG helper.
  return rand(seed);
}

// Power heuristic for two sampling strategies.
float MisMixWeight(float a, float b)
{
  // Uses the power heuristic for two competing strategies, usually light sampling vs BSDF sampling.
  const float aa = a * a;
  return aa / max(aa + b * b, 1.0e-6);
}

// PDF for cosine-weighted hemisphere sampling.
float PdfCosineHemisphere(float3 normal, float3 direction)
{
  // Returns a solid-angle PDF in world space.
  return clampedDot(normalize(normal), normalize(direction)) * kInvPi;
}

// Generates a cosine-weighted direction around the given shading normal.
float3 SampleCosineHemisphereDirection(float3 normal, inout uint seed, out float pdf)
{
  // Diffuse and sheen use the same cosine-weighted proposal because both are broad reflection lobes.
  const float2 xi       = float2(NextRandom(seed), NextRandom(seed));
  const float3 localDir = cosineSampleHemisphere(xi.x, xi.y);

  float3 tangent;
  float3 bitangent;
  orthonormalBasis(normal, tangent, bitangent);

  const float3 worldDir = normalize(tangent * localDir.x + bitangent * localDir.y + normal * localDir.z);
  pdf                   = PdfCosineHemisphere(normal, worldDir);
  return worldDir;
}

float3 ImportanceSampleGGX(float alpha, float2 xi)
{
  // Samples an isotropic GGX half vector in local space.
  const float a2        = alpha * alpha;
  const float cosThetaH = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
  const float sinThetaH = sqrt(max(0.0, 1.0 - cosThetaH * cosThetaH));
  const float phiH      = 6.28318530718 * xi.x;
  return float3(cos(phiH) * sinThetaH, sin(phiH) * sinThetaH, cosThetaH);
}

float3 ImportanceSampleVisibleGGX(float3 viewDirectionLocal, float alphaX, float alphaY, float2 xi)
{
  // Samples the visible normal distribution function so the microfacet proposals match the actual masking term better.
  const float3 stretchedView = normalize(float3(alphaX * viewDirectionLocal.x, alphaY * viewDirectionLocal.y, viewDirectionLocal.z));
  const float  lensq         = stretchedView.x * stretchedView.x + stretchedView.y * stretchedView.y;
  const float3 tangent1 =
      lensq > 0.0 ? float3(-stretchedView.y, stretchedView.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
  const float3 tangent2 = cross(stretchedView, tangent1);

  const float r   = sqrt(xi.x);
  const float phi = 6.28318530718 * xi.y;
  const float t1  = r * cos(phi);
  float       t2  = r * sin(phi);
  const float s   = 0.5 * (1.0 + stretchedView.z);
  t2              = lerp(sqrt(max(0.0, 1.0 - t1 * t1)), t2, s);

  const float3 normalHemisphere =
      t1 * tangent1 + t2 * tangent2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * stretchedView;

  return normalize(float3(alphaX * normalHemisphere.x, alphaY * normalHemisphere.y, max(0.0, normalHemisphere.z)));
}

float RescaleRandomNumber(float randomValue, float lowerBound, float upperBound)
{
  // Re-maps one uniform sample from a chosen CDF interval back into [0, 1) for nested branch sampling.
  const float oneMinusEpsilon = 0.99999994;
  return min((randomValue - lowerBound) / max(upperBound - lowerBound, 1.0e-6), oneMinusEpsilon);
}

#endif
