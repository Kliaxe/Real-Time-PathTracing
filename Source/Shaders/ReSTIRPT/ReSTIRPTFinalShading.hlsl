// Final shading
// ReSTIR PT Enhanced. Resolves the reservoir selected for each pixel into displayed radiance, and hands NRD its guide buffers and split radiance signals.
// This is a compute pass rather than a ray tracing one, which is a real difference from ReSTIR DI. A DI reservoir stores a light sample whose visibility is still unknown, so DI must trace a final shadow ray.
// A PT reservoir stores a path that was actually traced, and resampling carries its integrand along, so the shaded value is already fully determined: radiance is just F * ucw. That is also why PT has no final-visibility parameters.

#include <Common/ShaderTypes.h>
#include "NRD.hlsli"
#include "ShaderIncludes/DenoiserInputs.hlsli"
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PTGlobals.hlsli"
#include "ShaderIncludes/ReSTIR/Common.hlsli"
#include "ReSTIR/PTReservoir.hlsli"
#include "ReSTIR/PTReservoirStorage.hlsli"

// Denoiser constants
// Same values ReSTIR DI writes, so both renderers hand NRD signals on the same scale and a comparison between the two images is a comparison of the renderers.

static const float  kInvalidDenoiserViewZ          = 1.0e32;
static const float3 kMinDiffuseDemodulationFactor  = float3(0.05, 0.05, 0.05);
static const float3 kMinSpecularDemodulationFactor = float3(0.02, 0.02, 0.02);

// NRD's REBLUR path adds mv.xy directly to pixel UV, so XY is a UV delta.
// Local rather than shared with the path tracing headers: those declare their own bindings, and pulling them in here would drag ray tracing intrinsics into a compute entry point.
float3 ComputePTDenoiserMotionVector(float2 pixelCenter, float3 worldPosition, GltfSceneInfo sceneInfo)
{
  const float2 currentUv = pixelCenter / sceneInfo.viewportSize;

  const float4 previousClipPosition = mul(float4(worldPosition, 1.0), sceneInfo.prevViewProjMatrix);

  if(previousClipPosition.w <= 0.0)
  {
    return float3(0.0, 0.0, 0.0);
  }

  const float2 previousUv    = previousClipPosition.xy / previousClipPosition.w * 0.5 + 0.5;
  const float  currentViewZ  = mul(float4(worldPosition, 1.0), sceneInfo.viewMatrix).z;
  const float  previousViewZ = mul(float4(worldPosition, 1.0), sceneInfo.prevViewMatrix).z;

  return float3(previousUv - currentUv, previousViewZ - currentViewZ);
}

// A pixel with no primary hit still needs deterministic inputs: NRD reprojects every texel, so an uninitialized guide buffer is history corruption, not a gap.
void WriteInvalidDenoiserSignals(int2 pixel)
{
  motionVectorsImage[pixel]               = (float4)0.0;
  normalRoughnessImage[pixel]             = (float4)0.0;
  baseColorMetalnessImage[pixel]          = (float4)0.0;
  viewZImage[pixel]                       = kInvalidDenoiserViewZ;
  diffuseRadianceHitDistanceImage[pixel]  = (float4)0.0;
  specularRadianceHitDistanceImage[pixel] = (float4)0.0;
  specularDemodulationFactorImage[pixel]  = (float4)1.0;
}

// Splits one resampled radiance into the diffuse and specular signals NRD filters separately.
// The split is by ENERGY: initial sampling reports what fraction of the path it traced arrived through the specular lobe, and that fraction is applied to the resampled radiance. Splitting by the surface's material response instead would be smoother, but it would put specular energy into the diffuse signal on every rough metal and diffuse energy into the specular signal on every glossy dielectric, and the whole point of the two signals is that they are filtered with different kernels.
// The share is used only where the path collected energy; where it did not, the ratio is undefined and the material response is the only thing left to ask. Both halves are scaled from the same radiance, so their sum is unchanged: this separates the signal, it does not add or remove light.
// What the share cannot capture is that resampling may have replaced this pixel's path with a neighbour's while the share still describes this pixel's own initial sample. The spatial pass only accepts neighbours that pass the surface-similarity gate, so their lobe composition is close by construction, and the temporal pass reuses this same pixel. The residual is a bounded approximation, not a bias: the two signals are recombined by addition after denoising.
// The hit distances likewise come from this pixel's traced path, and describe the SCALE of the transport leaving it, which the initial sample measures directly.
void WriteDenoiserSignals(int2 pixel, ReSTIRPTSurface surface, float3 radiance, float3 denoiserGuide)
{
  if(surface.valid == 0u)
  {
    WriteInvalidDenoiserSignals(pixel);
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();
  const float         viewZ     = mul(float4(surface.worldPosition, 1.0), sceneInfo.viewMatrix).z;
  const float3        viewDir   = normalize(sceneInfo.cameraPosition - surface.worldPosition);
  const float3        rf0       = lerp(float3(0.04, 0.04, 0.04), surface.albedo, surface.metallic);

  // Demodulation factors
  // REBLUR filters material response out of the signal before blurring it.
  // Both factors are the ones NrdCompose reapplies, not the ones NRD_MaterialFactors returns for both lobes. Compose stores and replays the specular factor verbatim, but recomputes the diffuse one from base colour and metalness, so demodulating by NRD_MaterialFactors' diffuse output would divide by one quantity and multiply back by another, and the split below relies on the round trip being exact.

  float3 unusedDiffuseFactor;
  float3 specularFactor;

  NRD_MaterialFactors(surface.shadingNormal, viewDir, surface.albedo, rf0, surface.roughness, unusedDiffuseFactor, specularFactor);

  specularFactor = max(specularFactor, kMinSpecularDemodulationFactor);

  const float3 diffuseFactor = max(saturate(surface.albedo) * (1.0 - saturate(surface.metallic)), kMinDiffuseDemodulationFactor);

  // Guide buffers

  motionVectorsImage[pixel]              = float4(ComputePTDenoiserMotionVector((float2)pixel + 0.5, surface.worldPosition, sceneInfo), 0.0);
  normalRoughnessImage[pixel]            = NRD_FrontEnd_PackNormalAndRoughness(surface.shadingNormal, surface.roughness, 0.0);
  baseColorMetalnessImage[pixel]         = float4(saturate(surface.albedo), saturate(surface.metallic));
  viewZImage[pixel]                      = viewZ;
  specularDemodulationFactorImage[pixel] = float4(specularFactor, 1.0);

  // Energy split
  // materialShare is the fallback for a path that collected no energy: the specular share of the primary lobe's own response. Both factors are floored above, so this cannot divide by zero.
  // It decides nothing for a black pixel, but it keeps the two signals from being fed an arbitrary constant when reuse later brings light to one.

  const float diffuseWeight  = ReSTIRLuminance(diffuseFactor);
  const float specularWeight = ReSTIRLuminance(specularFactor);
  const float materialShare  = specularWeight / (diffuseWeight + specularWeight);
  const float specularShare  = denoiserGuide.z >= 0.0 ? denoiserGuide.z : materialShare;

  const float3 clampedRadiance  = max(radiance, (float3)0.0);
  // Demodulated, then clamped, in RTXPT's order: the clamp limits what NRD accumulates, so it acts on the signal NRD actually sees.
  const float3 diffuseRadiance  = ClampDenoiserRadiance(clampedRadiance * (1.0 - specularShare) / diffuseFactor, pushConst.denoiserRadianceClamp);
  const float3 specularRadiance = ClampDenoiserRadiance(clampedRadiance * specularShare / specularFactor, pushConst.denoiserRadianceClamp);

  // Hit distances
  // Zeros are passed through as zeros. NRD reserves a zero normalized hit distance for "this lobe was not sampled here" and reconstructs it from neighbours, but REBLUR_FrontEnd_GetNormHitDist clamps its result away from zero (it assumes the lobe WAS sampled), so the skipped case has to bypass it rather than be fed through it.

  const float diffuseNormHitDistance  = denoiserGuide.x > 0.0 ? REBLUR_FrontEnd_GetNormHitDist(denoiserGuide.x, viewZ, pushConst.reblurHitDistanceParams, 1.0) : 0.0;
  const float specularNormHitDistance = denoiserGuide.y > 0.0 ? REBLUR_FrontEnd_GetNormHitDist(denoiserGuide.y, viewZ, pushConst.reblurHitDistanceParams, surface.roughness) : 0.0;

  diffuseRadianceHitDistanceImage[pixel]  = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuseRadiance, diffuseNormHitDistance, true);
  specularRadianceHitDistanceImage[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specularRadiance, specularNormHitDistance, true);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 threadId: SV_DispatchThreadID)
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress.Get();
  const uint2         pixel     = threadId.xy;
  const uint2         viewport  = (uint2)sceneInfo.viewportSize;

  // The dispatch rounds up to whole workgroups, so edge threads must drop out.
  if(pixel.x >= viewport.x || pixel.y >= viewport.y)
  {
    return;
  }

  const uint pixelIndex = pixel.y * viewport.x + pixel.x;

  const ReSTIRPTReservoir reservoir = LoadPTReservoir(ptParams.reservoirBufferParams, PTPixelPosToReservoirPos(pixel), ptParams.bufferIndices.shadingInputBufferIndex);

  // Sample radiance
  // An invalid reservoir means no candidate in this pixel's path tree ever carried weight. That is legitimately black, not an error.

  float3 sampleRadiance = (float3)0.0;

  if(ptParams.shading.enableVectorWeights != 0u)
  {
    // Section 6.3. The resampling pass already summed the RGB contribution over every candidate, which removes the variance introduced by picking one index. Read unconditionally: the producing pass writes it for invalid reservoirs too, as black.
    sampleRadiance = SanitizeRadiance(ptShadingWeightBuffer[pixelIndex]);
  }
  else if(IsValidPTReservoir(reservoir))
  {
    sampleRadiance = SanitizeRadiance(EvaluatePTReservoirRadiance(reservoir));
  }

  // NRD consumes the split signals, not the resolved image, so this writes the pre-accumulation sample. Accumulation is a reference aid and is never on at the same time as denoising.
  if((pushConst.flags & uint(ReSTIRPTFlags::eReSTIRPTFlagWriteDenoiserSignals)) != 0u)
  {
    WriteDenoiserSignals((int2)pixel, currentSurfaceBuffer[pixelIndex], sampleRadiance, ptDenoiserGuideBuffer[pixelIndex]);
  }

  // Resolve
  // Running average over frames. This is presentation history and is unrelated to ReSTIR's own spatiotemporal reuse; both are reset together whenever the camera or scene changes.

  float3 resolvedRadiance = sampleRadiance;

  if((pushConst.flags & uint(ReSTIRPTFlags::eReSTIRPTFlagAccumulate)) != 0u && pushConst.accumulatedFrames > 0)
  {
    const float3 previousAverage = accumulationImage[(int2)pixel].xyz;
    const float  historyWeight   = float(pushConst.accumulatedFrames);

    resolvedRadiance = (previousAverage * historyWeight + sampleRadiance) / (historyWeight + 1.0);
  }

  accumulationImage[(int2)pixel] = float4(resolvedRadiance, 1.0);
  outImage[(int2)pixel]          = float4(resolvedRadiance, 1.0);
}

