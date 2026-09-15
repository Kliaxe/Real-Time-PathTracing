#define RTPT_HLSL 1

#include "Common/SkyIo.h"
#include "Sky.hlsli"

// SkyPushConstants
// Per-dispatch sky parameters; mirrors SkyRenderer::PushConstants on the CPU.

struct SkyPushConstants
{
  // Procedural sky description evaluated for every pixel.
  SkySimpleParameters parameters;

  // Inverse of projection times the rotation-only view, so NDC maps to a world direction with no camera translation.
  float4x4            transform;
};

[[vk::push_constant]] ConstantBuffer<SkyPushConstants> pushConstants;
[[vk::binding(0, 0), vk::image_format("rgba32f")]] RWTexture2D<float4> outputImage;

// 16x16 matches the group size SkyRenderer divides the extent by.
[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
  uint width;
  uint height;

  outputImage.GetDimensions(width, height);

  // The dispatch rounds up to whole groups, so edge invocations can fall outside the image.
  if(dispatchThreadId.x >= width || dispatchThreadId.y >= height)
  {
    return;
  }

  // View ray
  // The pixel centre becomes an NDC point and is unprojected into a world-space direction; only the direction is kept.

  const float2 screenPosition = ((float2)dispatchThreadId.xy + 0.5f) / float2(width, height) * 2.0f - 1.0f;
  const float4 transformed = mul(float4(screenPosition, 1.0f, 1.0f), pushConstants.transform);
  const float3 direction = normalize(transformed.xyz);

  outputImage[dispatchThreadId.xy] = float4(EvaluateSimpleSky(pushConstants.parameters, direction), 1.0f);
}
