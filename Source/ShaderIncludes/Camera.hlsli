#ifndef RTPT_CAMERA_HLSLI
#define RTPT_CAMERA_HLSLI

#include "Common/IoGltf.h"

// Unprojects an NDC position to a world-space view direction from the camera.
// Any point on the pixel's ray gives the same direction, so NDC depth 1 is used rather than a real depth.
float3 ReconstructWorldDirectionFromNdc(float2 ndc, GltfSceneInfo sceneInfo)
{
  const float4 worldPositionHomogeneous = mul(float4(ndc, 1.0f, 1.0f), sceneInfo.viewProjInvMatrix);

  // Skips the perspective divide when w is near zero instead of producing inf or NaN.
  const float divisor = abs(worldPositionHomogeneous.w) > 1.0e-6f ? worldPositionHomogeneous.w : 1.0f;

  return normalize(worldPositionHomogeneous.xyz / divisor - sceneInfo.cameraPosition);
}

// Maps a pixel coordinate (callers pass pixel centers) to a world-space view direction.
// Rows map to NDC Y without a flip; ComputeDenoiserMotionVector relies on the same convention.
float3 ReconstructWorldDirectionFromPixel(float2 pixelCoordinate, GltfSceneInfo sceneInfo)
{
  // The max keeps a zero-sized viewport from dividing by zero.
  const float2 viewport = max(sceneInfo.viewportSize, float2(1.0f, 1.0f));
  const float2 uv       = pixelCoordinate / viewport;

  return ReconstructWorldDirectionFromNdc(uv * 2.0f - 1.0f, sceneInfo);
}

#endif  // RTPT_CAMERA_HLSLI
