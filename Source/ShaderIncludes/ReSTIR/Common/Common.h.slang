#ifndef RESTIR_COMMON_H
#define RESTIR_COMMON_H

static const float kReSTIRPi = 3.14159265359;

float ReSTIRLuminance(float3 value)
{
  return dot(value, float3(0.2126, 0.7152, 0.0722));
}

float3 SanitizeRadiance(float3 value)
{
  value = select(isnan(value), float3(0.0), value);
  value = select(isinf(value), float3(0.0), value);
  return max(value, float3(0.0));
}

uint FlattenPixelIndex(uint2 pixelPosition, uint2 viewportSize)
{
  // All per-pixel storage uses the same linear addressing scheme.
  return pixelPosition.x + pixelPosition.y * viewportSize.x;
}

bool IsInsideViewport(int2 pixelPosition, uint2 viewportSize)
{
  return pixelPosition.x >= 0 && pixelPosition.y >= 0 && pixelPosition.x < int(viewportSize.x)
         && pixelPosition.y < int(viewportSize.y);
}

uint HashUint(uint value)
{
  // Small integer hash used for pass-local RNG seeds.
  value ^= 2747636419u;
  value *= 2654435769u;
  value ^= value >> 16;
  value *= 2654435769u;
  value ^= value >> 16;
  value *= 2654435769u;
  return value;
}

float RandomFloat(inout uint state)
{
  // The state is updated in place so a caller can advance one local stream
  // without carrying a larger RNG object.
  state = HashUint(state + 1u);
  return float(state & 0x00ffffffu) / float(0x01000000u);
}

float3 ReprojectWorldPositionToPixel(float3 worldPosition, GltfSceneInfo sceneInfo)
{
  // Temporal reuse projects the current receiver into previous-frame screen
  // space and returns pixel-center coordinates there.
  const float4 clipPosition = mul(float4(worldPosition, 1.0), sceneInfo.prevViewProjMatrix);
  if(clipPosition.w <= 0.0)
  {
    return float3(-1.0, -1.0, 0.0);
  }

  const float2 ndc = clipPosition.xy / clipPosition.w;
  if(any(abs(ndc) > 1.0))
  {
    return float3(-1.0, -1.0, 0.0);
  }

  const float2 viewportSize = sceneInfo.viewportSize;
  const float2 uv           = float2(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);
  return float3(uv * viewportSize - 0.5, clipPosition.z / clipPosition.w);
}

bool TryComputeReSTIRTemporalMotionVector(float2 pixelCenter,
                                          float3 worldPosition,
                                          float currentLinearDepth,
                                          GltfSceneInfo sceneInfo,
                                          out float3 motionVector)
{
  motionVector = float3(0.0, 0.0, 0.0);

  const float2 currentUv = pixelCenter / sceneInfo.viewportSize;
  const float4 previousClipPosition = mul(float4(worldPosition, 1.0), sceneInfo.prevViewProjMatrix);
  if(previousClipPosition.w <= 0.0)
  {
    return false;
  }

  const float2 previousNdc = previousClipPosition.xy / previousClipPosition.w;
  if(any(abs(previousNdc) > 1.0))
  {
    return false;
  }

  const float2 previousUv = previousNdc * 0.5 + 0.5;
  const float previousLinearDepth = length(worldPosition - sceneInfo.prevCameraPosition);
  motionVector = float3(previousUv - currentUv, previousLinearDepth - currentLinearDepth);
  return true;
}

float3 ConvertMotionVectorToPixelSpace(float2 pixelCenter, float3 motionVector, GltfSceneInfo sceneInfo)
{
  // The stored motion vector uses UV delta for XY so it can serve denoising and
  // temporal reprojection. ReSTIR temporal reuse needs a pixel-space offset.
  const float2 currentUv = pixelCenter / sceneInfo.viewportSize;
  const float2 previousUv = currentUv + motionVector.xy;
  const float2 previousPixelCenter = previousUv * sceneInfo.viewportSize;
  return float3(previousPixelCenter - pixelCenter, motionVector.z);
}

#endif
