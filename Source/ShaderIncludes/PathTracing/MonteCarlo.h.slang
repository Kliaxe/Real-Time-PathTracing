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
  // Exact Heitz VNDF sampling for anisotropic GGX in the local shading frame.
  const float3 stretchedView = normalize(float3(alphaX * viewDirectionLocal.x, alphaY * viewDirectionLocal.y, viewDirectionLocal.z));
  const float3 tangent1 =
      stretchedView.z < 0.99999 ? normalize(cross(stretchedView, float3(0.0, 0.0, 1.0))) : float3(1.0, 0.0, 0.0);
  const float3 tangent2 = cross(tangent1, stretchedView);

  const float a   = 1.0 / (1.0 + stretchedView.z);
  const float r   = sqrt(xi.x);
  const float phi = xi.y < a ? (xi.y / a) * 3.14159265359 : 3.14159265359 + ((xi.y - a) / max(1.0 - a, 1.0e-6)) * 3.14159265359;
  const float p1  = r * cos(phi);
  const float p2  = r * sin(phi) * (xi.y < a ? 1.0 : stretchedView.z);

  float3 halfVector = p1 * tangent1 + p2 * tangent2
                      + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * stretchedView;

  halfVector.x *= alphaX;
  halfVector.y *= alphaY;
  halfVector.z = max(0.0, halfVector.z);
  return normalize(halfVector);
}

float RescaleRandomNumber(float randomValue, float lowerBound, float upperBound)
{
  // Re-maps one uniform sample from a chosen CDF interval back into [0, 1) for nested branch sampling.
  const float oneMinusEpsilon = 0.99999994;
  return min((randomValue - lowerBound) / max(upperBound - lowerBound, 1.0e-6), oneMinusEpsilon);
}

#endif
