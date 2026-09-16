// Disocclusion threshold mix
// Builds NRD's IN_DISOCCLUSION_THRESHOLD_MIX: per pixel, how far REBLUR's disocclusion threshold moves from CommonSettings::disocclusionThreshold toward disocclusionThresholdAlternate.
// The measure is RTXPT's ComputeDisocclusionRelaxation (ProcessingPasses/PostProcess.hlsl): the normal divergence across the four direct neighbours. Where normals change quickly - creases, curved silhouettes, pixels next to the background - reprojected depth disagrees for reasons other than a real disocclusion, and the strict threshold throws away history it should have kept.
// RTXPT only applies it to surfaces seen through a mirror or glass, which its stable planes separate out. This renderer's guides always describe the primary hit, so here it applies to every surface.

#include "NRD.hlsli"

// Resources
// Bindings 0-2 must match the set layout and descriptor writes in NrdDisocclusionMixPass.

[[vk::binding(0, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(1, 0)]] Texture2D<float> inViewZ;
[[vk::binding(2, 0)]] [[vk::image_format("r16f")]] RWTexture2D<float> outDisocclusionThresholdMix;

// Constants
// kEdgeRelaxation is what a neighbour with no surface contributes, RTXPT's kEdge. The bias and scale map the summed divergence into [0, 1] exactly as RTXPT does.
// The renderers write a 1e32 viewZ where the primary ray missed; testing against a smaller threshold avoids an exact float compare.

static const float kEdgeRelaxation                = 0.02;
static const float kRelaxationBias                = 0.00002;
static const float kRelaxationScale               = 25.0;
static const float kInvalidDenoiserViewZThreshold = 1.0e31;

// One neighbour's contribution: 1 - cos of the angle between the two normals, or the edge constant when the neighbour has no surface.
float NeighbourRelaxation(int2 pixel, int2 offset, int2 imageSize, float3 centerNormal)
{
  const int2 neighbour = clamp(pixel + offset, int2(0, 0), imageSize - 1);

  if(inViewZ[neighbour] >= kInvalidDenoiserViewZThreshold)
  {
    return kEdgeRelaxation;
  }

  const float3 neighbourNormal = NRD_FrontEnd_UnpackNormalAndRoughness(inNormalRoughness[neighbour]).xyz;

  return 1.0 - dot(centerNormal, neighbourNormal);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
  uint width;
  uint height;

  outDisocclusionThresholdMix.GetDimensions(width, height);

  // The dispatch rounds up to whole groups, so edge invocations drop out.
  if(dispatchId.x >= width || dispatchId.y >= height)
  {
    return;
  }

  const int2 pixel     = int2(dispatchId.xy);
  const int2 imageSize = int2(width, height);

  // Background pixels are not denoised, so their threshold is irrelevant; zero keeps the image deterministic.
  if(inViewZ[pixel] >= kInvalidDenoiserViewZThreshold)
  {
    outDisocclusionThresholdMix[pixel] = 0.0;
    return;
  }

  const float3 centerNormal = NRD_FrontEnd_UnpackNormalAndRoughness(inNormalRoughness[pixel]).xyz;

  float relaxation = 0.0;

  relaxation += NeighbourRelaxation(pixel, int2(-1, 0), imageSize, centerNormal);
  relaxation += NeighbourRelaxation(pixel, int2(1, 0), imageSize, centerNormal);
  relaxation += NeighbourRelaxation(pixel, int2(0, -1), imageSize, centerNormal);
  relaxation += NeighbourRelaxation(pixel, int2(0, 1), imageSize, centerNormal);

  outDisocclusionThresholdMix[pixel] = saturate((relaxation - kRelaxationBias) * kRelaxationScale);
}
