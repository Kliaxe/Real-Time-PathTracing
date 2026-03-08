#ifndef CAMERA_H_SLANG
#define CAMERA_H_SLANG

#include <nvshaders/slang_types.h>
#include "Common/IoGltf.h"

// Reconstruct a world-space camera ray direction from normalized device
// coordinates. The scene buffer field is historically named projInvMatrix, but
// in this sample it stores the inverse view-projection matrix so both raster
// background shading and ray tracing can share the exact same camera contract.
float3 ReconstructWorldDirectionFromNdc(float2 ndc, GltfSceneInfo sceneInfo)
{
  float4 worldPosH = mul(float4(ndc, 1.0, 1.0), sceneInfo.projInvMatrix);
  float  w         = (abs(worldPosH.w) > 1e-6) ? worldPosH.w : 1.0;
  float3 worldPos  = worldPosH.xyz / w;
  return normalize(worldPos - sceneInfo.cameraPosition);
}

// Convenience wrapper for shaders that start from pixel coordinates instead of
// precomputed NDC. We clamp the viewport to at least 1x1 so the helper stays
// safe even during resize transitions.
float3 ReconstructWorldDirectionFromPixel(float2 pixelCoord, GltfSceneInfo sceneInfo)
{
  float2 viewport = max(sceneInfo.viewportSize, float2(1.0, 1.0));
  float2 uv       = pixelCoord / viewport;
  float2 ndc      = float2(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0);
  return ReconstructWorldDirectionFromNdc(ndc, sceneInfo);
}

#endif  // CAMERA_H_SLANG
