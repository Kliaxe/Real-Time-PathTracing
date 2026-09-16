// Ray Reconstruction inputs
// Turns the guide buffers and noisy radiance the renderers already write for NRD into the inputs DLSS Ray Reconstruction reads.
// The renderers keep writing one set of denoiser signals, and this pass does the conversion: unpacking NRD's normal encoding, splitting base colour into diffuse albedo, turning linear view depth into hardware depth, and deriving the motion Ray Reconstruction needs for reflections and the background.
// The only renderer-side difference is the specular hit distance, which the renderers store unnormalized while Ray Reconstruction is active, because REBLUR's normalization saturates exactly the long mirror distances reflection motion needs.

#include <Common/ShaderTypes.h>
#include "ShaderIo.h"
#include "NRD.hlsli"
#include "ShaderIncludes/Camera.hlsli"

// Resources
// Bindings 0-13 must match the set layout and descriptor writes in RayReconstructionInputPass. Inputs are the renderer's images, outputs are owned by the pass.

[[vk::binding(0, 0)]] Texture2D<float4> inColor;
[[vk::binding(1, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(2, 0)]] Texture2D<float4> inBaseColorMetalness;
[[vk::binding(3, 0)]] Texture2D<float> inViewZ;
[[vk::binding(4, 0)]] Texture2D<float4> inSpecularRadianceHitDistance;
[[vk::binding(5, 0)]] Texture2D<float4> inSpecularDemodulationFactor;
[[vk::binding(6, 0)]] Texture2D<float4> inMotionVectors;
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outColor;
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDiffuseAlbedo;
[[vk::binding(9, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outSpecularAlbedo;
[[vk::binding(10, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outNormalRoughness;
[[vk::binding(11, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> outDepth;
[[vk::binding(12, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outSpecularMotionVectors;
[[vk::binding(13, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outMotionVectors;

[[vk::push_constant]] ConstantBuffer<RayReconstructionInputsPushConstant> pushConst;

// Constants
// The renderers write a 1e32 viewZ where the primary ray missed; testing against a smaller threshold avoids an exact float compare.
// kMinAlbedo keeps the albedo guides from both reaching zero, RTXPT's guard: Ray Reconstruction divides lighting out by them, and a pixel with no reflectance at all gives it nothing to divide by.
// Reflections are only reprojected as mirror images below kSpecularMotionRoughness, RTXPT's hit-distance threshold. Rougher lobes spread their reflection too widely for one virtual image point to describe, and follow the surface instead.
// kHalfFloatMax is the largest value the half-float colour input can hold.

static const float kInvalidViewZThreshold    = 1.0e31;
static const float kMinAlbedo                = 0.05;
static const float kSpecularMotionRoughness  = 0.35;
static const float kHalfFloatMax             = 65504.0;

// Screen UV of a world-space point through a view-projection, and whether it lies in front of that camera. Rows map to UV without a flip, matching ReconstructWorldDirectionFromPixel.
bool ProjectToUv(float4 worldPoint, float4x4 viewProjection, out float2 uv)
{
  const float4 clip = mul(worldPoint, viewProjection);

  uv = clip.w > 0.0 ? clip.xy / clip.w * 0.5 + 0.5 : (float2)0.0;

  return clip.w > 0.0;
}

// Motion of the environment seen through this pixel. A direction has no position, so it is projected as a point at infinity (w = 0): only camera rotation moves it, which is exactly how a distant background behaves.
float2 ComputeBackgroundMotion(float3 direction, GltfSceneInfo sceneInfo)
{
  float2 currentUv;
  float2 previousUv;

  if(!ProjectToUv(float4(direction, 0.0), sceneInfo.viewProjMatrix, currentUv) || !ProjectToUv(float4(direction, 0.0), sceneInfo.prevViewProjMatrix, previousUv))
  {
    return (float2)0.0;
  }

  return previousUv - currentUv;
}

// Motion of a reflection, after RTXPT's ComputeSpecularMotionVector. The reflected hit is mirrored behind the reflecting plane into a virtual image point, and that point is reprojected: it moves the way the reflection moves as the camera does, which the surface's own motion does not.
// For a planar reflector the mirror image lies on the primary ray itself, one hit distance past the surface, so it needs no reflection basis. It assumes a static scene, and curved mirrors are only approximated.
float2 ComputeReflectionMotion(float3 surfacePosition, float3 viewDirection, float hitDistance, GltfSceneInfo sceneInfo)
{
  const float3 imagePosition = surfacePosition + viewDirection * hitDistance;

  float2 currentUv;
  float2 previousUv;

  if(!ProjectToUv(float4(imagePosition, 1.0), sceneInfo.viewProjMatrix, currentUv) || !ProjectToUv(float4(imagePosition, 1.0), sceneInfo.prevViewProjMatrix, previousUv))
  {
    return (float2)0.0;
  }

  return previousUv - currentUv;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
  uint width;
  uint height;

  outColor.GetDimensions(width, height);

  // The dispatch rounds up to whole groups, so edge invocations drop out.
  if(dispatchId.x >= width || dispatchId.y >= height)
  {
    return;
  }

  const int2          pixel     = int2(dispatchId.xy);
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();

  // Colour
  // Non-finite samples are zeroed so one bad path cannot poison the history. The clamp scales the brightest channel down to the ceiling, which keeps the hue of a firefly while limiting its energy.

  float3 color = inColor[pixel].rgb;

  color = any(isnan(color)) || any(isinf(color)) ? (float3)0.0 : max(color, (float3)0.0);

  const float brightestChannel = max(color.r, max(color.g, color.b));
  const float colorCeiling     = pushConst.radianceClamp > 0.0 ? min(pushConst.radianceClamp, kHalfFloatMax) : kHalfFloatMax;

  if(brightestChannel > colorCeiling)
  {
    color *= colorCeiling / brightestChannel;
  }

  outColor[pixel] = float4(color, 1.0);

  // The sample direction goes through the jittered sample position, the same one the renderer traced, so positions rebuilt along it land on the surface the pixel shows.
  const float2 samplePosition = GetPixelSamplePosition((uint2)pixel, sceneInfo);
  const float3 viewDirection  = ReconstructWorldDirectionFromPixel(samplePosition, sceneInfo);
  const float  viewZ          = inViewZ[pixel];

  // Background
  // No surface: the colour is the environment itself. Neutral guides tell Ray Reconstruction there is no material to separate, and far-plane depth places it behind everything.
  // The renderers write zero motion here because NRD never denoises the background, but Ray Reconstruction does, so the motion is replaced with camera rotation.

  if(viewZ >= kInvalidViewZThreshold)
  {
    const float2 backgroundMotion = ComputeBackgroundMotion(viewDirection, sceneInfo);

    outDiffuseAlbedo[pixel]         = float4(kMinAlbedo, kMinAlbedo, kMinAlbedo, 1.0);
    outSpecularAlbedo[pixel]        = float4(0.0, 0.0, 0.0, 1.0);
    outNormalRoughness[pixel]       = float4(-viewDirection, 1.0);
    outDepth[pixel]                 = 1.0;
    outSpecularMotionVectors[pixel] = float4(backgroundMotion, 0.0, 0.0);
    outMotionVectors[pixel]         = float4(backgroundMotion, 0.0, 0.0);

    return;
  }

  // Material guides
  // Diffuse albedo is the base colour metals do not scatter diffusely. Specular albedo is the pre-integrated specular response the renderer already computed for NRD's demodulation, so both denoisers see the same material.

  const float4 normalRoughness    = NRD_FrontEnd_UnpackNormalAndRoughness(inNormalRoughness[pixel]);
  const float4 baseColorMetalness = inBaseColorMetalness[pixel];

  float3       diffuseAlbedo  = baseColorMetalness.rgb * (1.0 - baseColorMetalness.a);
  const float3 specularAlbedo = inSpecularDemodulationFactor[pixel].rgb;

  if((dot(diffuseAlbedo + specularAlbedo, (float3)1.0) / 3.0) < kMinAlbedo)
  {
    diffuseAlbedo += kMinAlbedo;
  }

  outDiffuseAlbedo[pixel]   = float4(diffuseAlbedo, 1.0);
  outSpecularAlbedo[pixel]  = float4(specularAlbedo, 1.0);
  outNormalRoughness[pixel] = float4(normalRoughness.xyz, normalRoughness.w);

  // Depth
  // The surface position is rebuilt along the sample direction from its view depth, then projected like a rasterizer would: Ray Reconstruction expects the [0, 1] hardware depth the motion vectors were made from.

  const float  directionViewZ  = mul(float4(viewDirection, 0.0), sceneInfo.viewMatrix).z;
  const float  distanceAlong   = abs(directionViewZ) > 1.0e-6 ? viewZ / directionViewZ : 0.0;
  const float3 surfacePosition = sceneInfo.cameraPosition + viewDirection * distanceAlong;
  const float4 surfaceClip     = mul(float4(surfacePosition, 1.0), sceneInfo.viewProjMatrix);

  outDepth[pixel] = surfaceClip.w > 0.0 ? saturate(surfaceClip.z / surfaceClip.w) : 1.0;

  // Reflection motion
  // The renderer stores the raw specular hit distance in the specular signal's alpha while Ray Reconstruction is active. Zero means the path did not take the specular lobe this frame, and a rough lobe has no single image point; both keep the surface motion.
  // A reflected ray that escaped is recorded at the half-float maximum. Its mirror image lies at infinity along the primary ray, so it moves like the background seen in that direction.

  const float  specularHitDistance = inSpecularRadianceHitDistance[pixel].a;
  const float2 surfaceMotion       = inMotionVectors[pixel].xy;

  const bool mirrorLike       = specularHitDistance > 1.0e-3 && normalRoughness.w < kSpecularMotionRoughness;
  const bool reflectsDistance = !(specularHitDistance < kHalfFloatMax);

  float2 reflectionMotion = surfaceMotion;

  if(mirrorLike)
  {
    reflectionMotion = reflectsDistance ? ComputeBackgroundMotion(viewDirection, sceneInfo) : ComputeReflectionMotion(surfacePosition, viewDirection, specularHitDistance, sceneInfo);
  }

  outSpecularMotionVectors[pixel] = float4(reflectionMotion, 0.0, 0.0);
  outMotionVectors[pixel]         = float4(surfaceMotion, 0.0, 0.0);
}
