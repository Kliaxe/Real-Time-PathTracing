#include "NRD.hlsli"

// Resources
// Bindings 0-10 must match the compose set layout and descriptor writes in NrdComposePass.
// Denoised outputs first, then the renderer's raw beauty and guides, then the noisy inputs for the debug views, then the output target.

[[vk::binding(0, 0)]] Texture2D<float4> inDiffuseRadianceHitDistance;
[[vk::binding(1, 0)]] Texture2D<float4> inSpecularRadianceHitDistance;
[[vk::binding(2, 0)]] Texture2D<float4> inRawBeauty;
[[vk::binding(3, 0)]] Texture2D<float> inViewZ;
[[vk::binding(4, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(5, 0)]] Texture2D<float4> inMotionVectors;
[[vk::binding(6, 0)]] Texture2D<float4> inNoisyDiffuseRadianceHitDistance;
[[vk::binding(7, 0)]] Texture2D<float4> inNoisySpecularRadianceHitDistance;
[[vk::binding(8, 0)]] Texture2D<float4> inBaseColorMetalness;
[[vk::binding(9, 0)]] Texture2D<float4> inSpecularDemodulationFactor;
[[vk::binding(10, 0)]] RWTexture2D<float4> outImage;

// Constants
// The renderers write a 1e32 viewZ where the primary ray missed; testing against a smaller threshold avoids an exact float compare.
// The minimum demodulation factors are the same clamps the path tracer applies before dividing, so remodulation never multiplies by less than was divided out.

static const float kInvalidDenoiserViewZThreshold = 1.0e31;
static const float3 kMinDiffuseDemodulationFactor = float3(0.05, 0.05, 0.05);
static const float3 kMinSpecularDemodulationFactor = float3(0.02, 0.02, 0.02);

// ResolvePushConstants
// Per-dispatch parameters; mirrors NrdComposePass::PushConstants in NrdComposePass.h.

struct ResolvePushConstants
{
  // DenoiserDebugView value selecting what is written to the output.
  uint debugView;

  // Nonzero when the noisy signals were divided by material factors and must be remodulated.
  uint useMaterialDemodulation;
};

[[vk::push_constant]] ConstantBuffer<ResolvePushConstants> pushConstants;

float3 VisualizeNormalRoughness(float4 packedNormalRoughness)
{
  const float4 unpacked = NRD_FrontEnd_UnpackNormalAndRoughness(packedNormalRoughness);

  // Map the [-1, 1] normal into displayable [0, 1].
  return 0.5 * unpacked.xyz + 0.5;
}

float3 VisualizeViewZ(float viewZ)
{
  // Background pixels carry the miss sentinel, which would otherwise saturate to white.
  if(viewZ >= kInvalidDenoiserViewZThreshold)
  {
    return float3(0.0, 0.0, 0.0);
  }

  // d / (d + 10) compresses unbounded depth into [0, 1) without a scene-specific far plane.
  const float distance = abs(viewZ);
  const float normalized = saturate(distance / (distance + 10.0));

  return normalized.xxx;
}

float3 VisualizeMotion(float4 motion)
{
  // Motion is a screen-UV delta, so zero motion shows as mid grey.
  return saturate(float3(motion.xy * 0.5 + 0.5, 0.5));
}

float3 ComputeDiffuseDemodulationFactor(float4 baseColorMetalness)
{
  // Diffuse factor
  // Rebuilt from the base colour / metalness guide instead of being stored, which is why that image is still produced after NRD 4.17 stopped reading it.

  const float3 diffuseFactor = saturate(baseColorMetalness.rgb) * (1.0 - saturate(baseColorMetalness.a));

  return max(diffuseFactor, kMinDiffuseDemodulationFactor);
}

float3 ComputeSpecularDemodulationFactor(float4 packedSpecularFactor)
{
  // The ray tracing passes computed NRD_MaterialFactors with the exact primary-view vector.
  // Compose only needs to reapply the stored factor after REBLUR has filtered pure specular radiance.
  return max(packedSpecularFactor.rgb, kMinSpecularDemodulationFactor);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
  uint width;
  uint height;

  outImage.GetDimensions(width, height);

  // The dispatch rounds up to whole 8x8 groups, so edge invocations can fall outside the image.
  if(dispatchThreadID.x >= width || dispatchThreadID.y >= height)
  {
    return;
  }

  // Load inputs

  const int3 imagePosition = int3(dispatchThreadID.xy, 0);
  const float4 diffusePacked  = inDiffuseRadianceHitDistance.Load(imagePosition);
  const float4 specularPacked = inSpecularRadianceHitDistance.Load(imagePosition);
  const float4 rawBeauty      = inRawBeauty.Load(imagePosition);
  const float  viewZ          = inViewZ.Load(int3(dispatchThreadID.xy, 0));
  const float4 normalRoughness = inNormalRoughness.Load(imagePosition);
  const float4 motionVectors   = inMotionVectors.Load(imagePosition);
  const float4 noisyDiffusePacked  = inNoisyDiffuseRadianceHitDistance.Load(imagePosition);
  const float4 noisySpecularPacked = inNoisySpecularRadianceHitDistance.Load(imagePosition);
  const float4 baseColorMetalness  = inBaseColorMetalness.Load(imagePosition);
  const float4 specularDemodulationFactorPacked = inSpecularDemodulationFactor.Load(imagePosition);

  // Unpack and remodulate
  // REBLUR stores radiance in YCoCg, so both the denoised outputs and the noisy inputs are unpacked before use.
  // Negative radiance is clamped before remodulation. The noisy signals go through the same path so the input debug views are comparable with the denoised ones.

  const float4 diffuseResolved      = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffusePacked);
  const float4 specularResolved     = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specularPacked);
  const float4 noisyDiffuseResolved = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(noisyDiffusePacked);
  const float4 noisySpecularResolved = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(noisySpecularPacked);
  const float3 diffuseDemodulationFactor = ComputeDiffuseDemodulationFactor(baseColorMetalness);
  const float3 specularDemodulationFactor = ComputeSpecularDemodulationFactor(specularDemodulationFactorPacked);
  const float3 remodulatedDiffuse = (pushConstants.useMaterialDemodulation != 0u) ? max(diffuseResolved.rgb, float3(0.0, 0.0, 0.0)) * diffuseDemodulationFactor : max(diffuseResolved.rgb, float3(0.0, 0.0, 0.0));
  const float3 remodulatedSpecular = (pushConstants.useMaterialDemodulation != 0u) ? max(specularResolved.rgb, float3(0.0, 0.0, 0.0)) * specularDemodulationFactor : max(specularResolved.rgb, float3(0.0, 0.0, 0.0));
  const float3 noisyRemodulatedDiffuse = (pushConstants.useMaterialDemodulation != 0u) ? max(noisyDiffuseResolved.rgb, float3(0.0, 0.0, 0.0)) * diffuseDemodulationFactor : max(noisyDiffuseResolved.rgb, float3(0.0, 0.0, 0.0));
  const float3 noisyRemodulatedSpecular = (pushConstants.useMaterialDemodulation != 0u) ? max(noisySpecularResolved.rgb, float3(0.0, 0.0, 0.0)) * specularDemodulationFactor : max(noisySpecularResolved.rgb, float3(0.0, 0.0, 0.0));
  const float3 combinedRadiance     = remodulatedDiffuse + remodulatedSpecular;

  // Mask background pixels
  // A pixel with no primary hit carries the sky marker in viewZ. NRD skips those entirely, so its output images keep whatever they last held rather than being cleared, which shows up in the signal debug views as coloured fringing over the background.
  // There is no surface there and therefore no diffuse or specular signal, so the four signal views report black instead of stale denoiser memory.

  const bool   hasSurface = viewZ < kInvalidDenoiserViewZThreshold;
  const float3 surfaceOnlyDiffuse       = hasSurface ? remodulatedDiffuse : float3(0.0, 0.0, 0.0);
  const float3 surfaceOnlySpecular      = hasSurface ? remodulatedSpecular : float3(0.0, 0.0, 0.0);
  const float3 surfaceOnlyNoisyDiffuse  = hasSurface ? noisyRemodulatedDiffuse : float3(0.0, 0.0, 0.0);
  const float3 surfaceOnlyNoisySpecular = hasSurface ? noisyRemodulatedSpecular : float3(0.0, 0.0, 0.0);

  // Select output
  // Case values follow DenoiserDebugView.

  float3 outputRadiance = combinedRadiance;

  switch(pushConstants.debugView)
  {
    // Final and denoised beauty: the denoised sum, with raw beauty filling the background NRD does not denoise.
    case 0u:
    case 2u:
    {
      if(viewZ >= kInvalidDenoiserViewZThreshold)
      {
        outputRadiance = rawBeauty.rgb;
      }
      break;
    }
    // Raw beauty.
    case 1u:
      outputRadiance = rawBeauty.rgb;
      break;
    // Noisy diffuse input.
    case 3u:
      outputRadiance = surfaceOnlyNoisyDiffuse;
      break;
    // Noisy specular input.
    case 4u:
      outputRadiance = surfaceOnlyNoisySpecular;
      break;
    // Denoised diffuse.
    case 5u:
      outputRadiance = surfaceOnlyDiffuse;
      break;
    // Denoised specular.
    case 6u:
      outputRadiance = surfaceOnlySpecular;
      break;
    case 7u:
      outputRadiance = VisualizeNormalRoughness(normalRoughness);
      break;
    case 8u:
      outputRadiance = VisualizeViewZ(viewZ);
      break;
    case 9u:
      outputRadiance = VisualizeMotion(motionVectors);
      break;
    default:
      outputRadiance = combinedRadiance;
      break;
  }

  outImage[(int2)dispatchThreadID.xy] = float4(outputRadiance, 1.0);
}
