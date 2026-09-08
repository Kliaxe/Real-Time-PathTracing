#ifndef RESTIR_COMMON_H
#define RESTIR_COMMON_H

// Small math and reprojection helpers shared by the ReSTIR passes. Keep this
// file renderer-agnostic: it should not know about reservoirs, light sampling,
// or descriptor bindings.

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

// Zero or negative threshold makes the surface-similarity test always succeed.
bool CompareRelativeDifference(float reference, float candidate, float threshold)
{
  return (threshold <= 0) || abs(reference - candidate) <= threshold * max(reference, candidate);
}

// Material similarity gate for spatial reuse.
//
// Normal and depth agreement is not sufficient on its own: two pixels can sit on
// the same flat wall yet straddle a texture or material boundary, and a path
// anchored to one material shifts poorly onto the other. Rejecting on material as
// well is cheap next to the shift it avoids.
//
// Compares the terms that actually drive the shift's BSDF evaluation - roughness
// and metalness decide the lobe, albedo decides the throughput.
bool IsMaterialSimilar(float ourRoughness, float theirRoughness, float ourMetallic, float theirMetallic,
                       float3 ourAlbedo, float3 theirAlbedo)
{
  const float kRoughnessTolerance = 0.25;
  const float kMetallicTolerance  = 0.25;
  const float kAlbedoTolerance    = 0.25;

  if(abs(ourRoughness - theirRoughness) > kRoughnessTolerance)
  {
    return false;
  }
  if(abs(ourMetallic - theirMetallic) > kMetallicTolerance)
  {
    return false;
  }

  // Relative albedo difference, so dark materials are not held to an absolute
  // tolerance that they can never exceed.
  const float3 albedoDelta = abs(ourAlbedo - theirAlbedo);
  const float3 albedoScale = max(max(ourAlbedo, theirAlbedo), float3(1.0e-3));
  return max(max(albedoDelta.x / albedoScale.x, albedoDelta.y / albedoScale.y), albedoDelta.z / albedoScale.z)
         <= kAlbedoTolerance;
}

bool IsValidNeighbor(float3 ourNorm, float3 theirNorm, float ourDepth, float theirDepth, float normalThreshold, float depthThreshold)
{
  return dot(theirNorm.xyz, ourNorm.xyz) >= normalThreshold && CompareRelativeDifference(ourDepth, theirDepth, depthThreshold);
}

bool TryComputeReSTIRTemporalMotionVector(float2 pixelCenter,
                                          float3 worldPosition,
                                          float currentLinearDepth,
                                          GltfSceneInfo sceneInfo,
                                          out float3 motionVector)
{
  // Reprojects the current world-space hit into the previous camera. The
  // returned XY is a UV delta, while Z stores the linear-depth delta used by
  // temporal surface validation.
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
