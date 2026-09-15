#ifndef PATH_TRACING_UTILITY_H
#define PATH_TRACING_UTILITY_H

#include "ShaderIncludes/Math.hlsli"
#include "ShaderIncludes/PathTracing/Common.hlsli"

// Common utilities
// Ray origin offsetting, tangent-frame conversions, and CDF lookup shared by the lighting and BSDF code.

// Largest channel. Used for "is any channel non-zero?" style tests without branching per component.
float SafeMax3(float3 value)
{
  return max(value.x, max(value.y, value.z));
}

// Offsets a ray origin off a surface to avoid self-intersection.
// This is the method from Ray Tracing Gems chapter 6 ("A Fast and Robust Method for Avoiding Self-Intersection"); the kRayOrigin* constants in Common.hlsli are its parameters.
float3 OffsetRay(float3 position, float3 geometricNormal)
{
  // Integer offset
  // The integer path nudges the floating-point bit pattern directly, which keeps the offset scale proportional to magnitude.
  // The offset moves away from zero on each axis, following the normal's sign.

  const int3 offsetInt = int3(int(kRayOriginIntScale * geometricNormal.x), int(kRayOriginIntScale * geometricNormal.y), int(kRayOriginIntScale * geometricNormal.z));

  const float3 offsetPosition = float3(asfloat(asint(position.x) + ((position.x < 0.0) ? -offsetInt.x : offsetInt.x)), asfloat(asint(position.y) + ((position.y < 0.0) ? -offsetInt.y : offsetInt.y)), asfloat(asint(position.z) + ((position.z < 0.0) ? -offsetInt.z : offsetInt.z)));

  // Near origin
  // Close to zero, a few ULPs are too small to escape the surface, so a fixed floating-point epsilon along the normal is used instead.

  return float3(abs(position.x) < kRayOriginFloor ? position.x + kRayOriginFloatEps * geometricNormal.x : offsetPosition.x, abs(position.y) < kRayOriginFloor ? position.y + kRayOriginFloatEps * geometricNormal.y : offsetPosition.y, abs(position.z) < kRayOriginFloor ? position.z + kRayOriginFloatEps * geometricNormal.z : offsetPosition.z);
}

// Flips the geometric normal to the side the ray leaves on.
// The shading frame may model a smoother surface than the coarse triangle, so the offset must follow the sampled ray side.
float3 SelectOffsetNormal(float3 geometricNormal, float3 direction)
{
  return dot(geometricNormal, direction) >= 0.0 ? geometricNormal : -geometricNormal;
}

// Builds an orthonormal basis on demand when no authored tangent frame exists.
// Rows are tangent, bitangent, normal, so mul(frame, v) projects into local space.
float3x3 GetTangentSpace(float3 normal)
{
  float3 tangent;
  float3 bitangent;

  BuildOrthonormalBasis(normal, tangent, bitangent);

  return float3x3(tangent, bitangent, normal);
}

// Reuses the resolved tangent frame when the caller already computed one.
float3x3 GetTangentSpace(float3 normal, float3 tangent, float3 bitangent)
{
  return float3x3(tangent, bitangent, normal);
}

// Converts a world-space vector into the implicit basis built from the provided normal.
float3 ToLocal(float3 normal, float3 direction)
{
  return mul(GetTangentSpace(normal), direction);
}

// Converts a world-space vector into the explicit tangent frame used by anisotropic BRDF code.
float3 ToLocal(float3 normal, float3 tangent, float3 bitangent, float3 direction)
{
  return mul(GetTangentSpace(normal, tangent, bitangent), direction);
}

// Converts a local-frame vector back into world space. The basis is orthonormal, so its transpose is its inverse.
float3 ToWorld(float3 normal, float3 direction)
{
  return mul(transpose(GetTangentSpace(normal)), direction);
}

// Converts a vector in the caller-provided tangent frame back into world space.
float3 ToWorld(float3 normal, float3 tangent, float3 bitangent, float3 direction)
{
  return mul(transpose(GetTangentSpace(normal, tangent, bitangent)), direction);
}

// Finds the first CDF entry >= value, which is exactly what the flat light samplers need.
// The CDF is a device buffer of 4-byte floats. A value past the last entry resolves to the last index.
uint BinarySearchCdf(uint64_t cdfAddress, uint count, float value)
{
  uint low  = 0;
  uint high = count > 0 ? (count - 1) : 0;

  while(low < high)
  {
    const uint mid = (low + high) >> 1;

    if(value <= vk::RawBufferLoad<float>(cdfAddress + uint64_t(mid) * 4u, 4))
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

