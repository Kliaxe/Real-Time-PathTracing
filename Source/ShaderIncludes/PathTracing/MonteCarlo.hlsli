#ifndef PATH_TRACING_MONTE_CARLO_H
#define PATH_TRACING_MONTE_CARLO_H

#include "ShaderIncludes/Random.hlsli"
#include "ShaderIncludes/Math.hlsli"
#include "ShaderIncludes/PathTracing/Common.hlsli"

// Random numbers and importance sampling
// The tracer's RNG entry point, the MIS weight, and the direction samplers used by the BSDF and environment code.

// Central wrapper so the rest of the tracer does not depend directly on the underlying RNG helper.
float NextRandom(inout uint seed)
{
  return RandomFloat(seed);
}

// Power heuristic for two competing strategies, usually light sampling vs BSDF sampling.
// Returns the weight of strategy a. Both arguments are densities in the same measure, solid angle at every call site.
// Invariant: MisMixWeight(a, b) + MisMixWeight(b, a) is one, so the two estimators of the same path neither lose nor double count energy.
float MisMixWeight(float a, float b)
{
  const float aa  = a * a;
  const float sum = aa + b * b;

  // The direct form is kept wherever its denominator is well conditioned, which is nearly every sample, so those weights are unchanged bit for bit.
  if(sum >= 1.0e-6)
  {
    return aa / sum;
  }

  // Tiny densities
  // Below this point the squares approach underflow. Flooring the denominator instead would pull both weights of the pair below one together and lose energy wherever both densities are small.
  // Dividing through by a^2 gives the same weight from the ratio of the densities, which stays representable, and the pair still sums to one.

  if(a <= 0.0)
  {
    return 0.0;
  }

  if(b <= 0.0)
  {
    return 1.0;
  }

  const float ratio = b / a;

  return 1.0 / (1.0 + ratio * ratio);
}

// PDF for cosine-weighted hemisphere sampling, as a world-space solid-angle PDF.
float PdfCosineHemisphere(float3 normal, float3 direction)
{
  return ClampedDot(normalize(normal), normalize(direction)) * kInvPi;
}

// Cosine-weighted direction around the given normal from a caller-supplied 2D sample.
// Diffuse and sheen use the same cosine-weighted proposal because both are broad reflection lobes.
float3 SampleCosineHemisphereDirection(float3 normal, float2 xi, out float pdf)
{
  const float3 localDir = CosineSampleHemisphere(xi.x, xi.y);

  float3 tangent;
  float3 bitangent;

  BuildOrthonormalBasis(normal, tangent, bitangent);

  const float3 worldDir = normalize(tangent * localDir.x + bitangent * localDir.y + normal * localDir.z);
  pdf                   = PdfCosineHemisphere(normal, worldDir);

  return worldDir;
}

// Generates a cosine-weighted direction around the given shading normal, drawing its two randoms from the seed.
float3 SampleCosineHemisphereDirection(float3 normal, inout PathSampleStream seed, out float pdf)
{
  return SampleCosineHemisphereDirection(normal, NextRandom2(seed), pdf);
}

// Samples an isotropic GGX half vector in local space, distributed as D(h) * cos(theta_h).
float3 ImportanceSampleGGX(float alpha, float2 xi)
{
  const float a2        = alpha * alpha;
  const float cosThetaH = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
  const float sinThetaH = sqrt(max(0.0, 1.0 - cosThetaH * cosThetaH));
  const float phiH      = 6.28318530718 * xi.x;

  return float3(cos(phiH) * sinThetaH, sin(phiH) * sinThetaH, cosThetaH);
}

// Exact Heitz VNDF sampling for anisotropic GGX in the local shading frame.
// The half vector is distributed as G1(v) * max(0, v.h) * D(h) / (n.v), which is the density EvaluateMicrofacetReflection reports.
float3 ImportanceSampleVisibleGGX(float3 viewDirectionLocal, float alphaX, float alphaY, float2 xi)
{
  // Hemisphere configuration
  // Stretching the view by the roughness turns the problem into sampling the visible normals of a unit hemisphere. The tangent1 fallback covers a view along +Z, where the cross product vanishes.

  const float3 stretchedView = normalize(float3(alphaX * viewDirectionLocal.x, alphaY * viewDirectionLocal.y, viewDirectionLocal.z));
  const float3 tangent1      = stretchedView.z < 0.99999 ? normalize(cross(stretchedView, float3(0.0, 0.0, 1.0))) : float3(1.0, 0.0, 0.0);
  const float3 tangent2      = cross(tangent1, stretchedView);

  // Projected disk sample
  // The disk is split into a full half and a half foreshortened by stretchedView.z. `a` is the probability of the first half, which is its share of the projected area.

  const float a   = 1.0 / (1.0 + stretchedView.z);
  const float r   = sqrt(xi.x);
  const float phi = xi.y < a ? (xi.y / a) * 3.14159265359 : 3.14159265359 + ((xi.y - a) / max(1.0 - a, 1.0e-6)) * 3.14159265359;
  const float p1  = r * cos(phi);
  const float p2  = r * sin(phi) * (xi.y < a ? 1.0 : stretchedView.z);

  // Unstretch
  // Lift the disk point onto the hemisphere, then undo the roughness stretch to get the ellipsoid's normal.

  float3 halfVector = p1 * tangent1 + p2 * tangent2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * stretchedView;

  halfVector.x *= alphaX;
  halfVector.y *= alphaY;
  halfVector.z  = max(0.0, halfVector.z);

  return normalize(halfVector);
}

// Re-maps one uniform sample from a chosen CDF interval back into [0, 1) for nested branch sampling.
// The upper clamp keeps the result strictly below one, even when the input is 1.0.
float RescaleRandomNumber(float randomValue, float lowerBound, float upperBound)
{
  const float oneMinusEpsilon = 0.99999994;

  return min((randomValue - lowerBound) / max(upperBound - lowerBound, 1.0e-6), oneMinusEpsilon);
}

#endif

