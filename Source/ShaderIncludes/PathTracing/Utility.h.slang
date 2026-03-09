#ifndef PATH_TRACING_UTILITY_H
#define PATH_TRACING_UTILITY_H

#include "nvshaders/functions.h.slang"
#include "ShaderIncludes/PathTracing/Common.h.slang"

// -------------------------------------------------------------------------
//    Common utilities
// -------------------------------------------------------------------------

float SafeMax3(float3 value)
{
  // Small helper used for "is any channel non-zero?" style tests without branching per component.
  return max(value.x, max(value.y, value.z));
}

// Ray Tracing Gems style origin offset.
float3 OffsetRay(float3 position, float3 geometricNormal)
{
  // The integer path nudges the floating-point bit pattern directly, which keeps the offset scale proportional to magnitude.
  const int3 offsetInt = int3(int(kRayOriginIntScale * geometricNormal.x), int(kRayOriginIntScale * geometricNormal.y),
                              int(kRayOriginIntScale * geometricNormal.z));

  const float3 offsetPosition = float3(
      asfloat(asint(position.x) + ((position.x < 0.0) ? -offsetInt.x : offsetInt.x)),
      asfloat(asint(position.y) + ((position.y < 0.0) ? -offsetInt.y : offsetInt.y)),
      asfloat(asint(position.z) + ((position.z < 0.0) ? -offsetInt.z : offsetInt.z)));

  return float3(abs(position.x) < kRayOriginFloor ? position.x + kRayOriginFloatEps * geometricNormal.x : offsetPosition.x,
                abs(position.y) < kRayOriginFloor ? position.y + kRayOriginFloatEps * geometricNormal.y : offsetPosition.y,
                abs(position.z) < kRayOriginFloor ? position.z + kRayOriginFloatEps * geometricNormal.z : offsetPosition.z);
}

float3 SelectOffsetNormal(float3 geometricNormal, float3 direction)
{
  // The shading frame may model a smoother surface than the coarse triangle, so the offset must follow the sampled ray side.
  return dot(geometricNormal, direction) >= 0.0 ? geometricNormal : -geometricNormal;
}

float3x3 GetTangentSpace(float3 normal)
{
  // Builds an orthonormal basis on demand when no authored tangent frame exists.
  float3 tangent;
  float3 bitangent;
  orthonormalBasis(normal, tangent, bitangent);
  return float3x3(tangent, bitangent, normal);
}

float3x3 GetTangentSpace(float3 normal, float3 tangent, float3 bitangent)
{
  // Reuses the resolved tangent frame when the caller already computed one.
  return float3x3(tangent, bitangent, normal);
}

float3 ToLocal(float3 normal, float3 direction)
{
  // Converts a world-space vector into the implicit basis built from the provided normal.
  return mul(GetTangentSpace(normal), direction);
}

float3 ToLocal(float3 normal, float3 tangent, float3 bitangent, float3 direction)
{
  // Converts a world-space vector into the explicit tangent frame used by anisotropic BRDF code.
  return mul(GetTangentSpace(normal, tangent, bitangent), direction);
}

float3 ToWorld(float3 normal, float3 direction)
{
  // Converts a local-frame vector back into world space.
  return mul(transpose(GetTangentSpace(normal)), direction);
}

float3 ToWorld(float3 normal, float3 tangent, float3 bitangent, float3 direction)
{
  // Converts a local-frame vector back into the caller-provided tangent frame.
  return mul(transpose(GetTangentSpace(normal, tangent, bitangent)), direction);
}

float Luminance(float3 rgb)
{
  // Shared Rec. 709-style luminance used for lobe probabilities and light weights.
  return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

uint BinarySearchCdf(float* cdf, uint count, float value)
{
  // Finds the first CDF entry >= value, which is exactly what the flat light samplers need.
  uint low  = 0;
  uint high = count > 0 ? (count - 1) : 0;

  while(low < high)
  {
    const uint mid = (low + high) >> 1;
    if(value <= cdf[mid])
    {
      high = mid;
    }
    else
    {
      low = mid + 1;
    }
  }

  return low;
}

#endif
