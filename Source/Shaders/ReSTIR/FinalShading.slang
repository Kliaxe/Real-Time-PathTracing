#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "NRD.hlsli"
#include "ShaderIncludes/ReSTIR/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/Common.h.slang"
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Continuation.h.slang"
#include "ShaderIncludes/ReSTIR/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/Debug.h.slang"

static const float kInvalidDenoiserViewZ      = 1.0e32;
static const float kDenoiserMissHitDistance   = 65504.0;
static const float4 kReblurHitDistanceParameters = float4(3.0, 0.1, 20.0, -25.0);
static const float3 kMinDiffuseDemodulationFactor = float3(0.05, 0.05, 0.05);
static const float3 kMinSpecularDemodulationFactor = float3(0.02, 0.02, 0.02);

// Final ReSTIR ray tracing pass. The ray generation shader shades the selected
// reservoir, while closest-hit/miss shaders are only used by optional secondary
// path bounces after direct lighting has been resolved.

float4 PackNrdNormalRoughness(float3 shadingNormal, float roughness)
{
  // NRD expects normals and roughness in its frontend packing convention.
  return NRD_FrontEnd_PackNormalAndRoughness(shadingNormal, roughness, 0.0);
}

float4 PackReblurRadianceHitDistance(float3 radiance, float hitDistance, float viewZ, float roughness, bool isDiffuse)
{
  // NRD stores hit distance in a normalized form so diffuse/specular denoising can reason about scale.
  const float normalizedHitDistance =
      REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, kReblurHitDistanceParameters, isDiffuse ? 1.0 : roughness);
  return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normalizedHitDistance, true);
}

float4 PackBaseColorMetalness(float3 baseColor, float metalness)
{
  // Base color and metalness are demodulation hints for the denoiser.
  return float4(saturate(baseColor), saturate(metalness));
}

void ComputeDenoiserMaterialFactors(DISurface surface, out float3 diffuseFactor, out float3 specularFactor)
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const float3 viewDir = normalize(sceneInfo.cameraPosition - surface.worldPosition);
  const float3 rf0 = lerp(float3(0.04, 0.04, 0.04), surface.albedo, surface.metallic);

  // REBLUR works best when material response is removed before filtering.
  // The specular factor is stored separately because the compose pass cannot
  // reconstruct the exact primary view vector without extra scene inputs.
  NRD_MaterialFactors(surface.shadingNormal, viewDir, surface.albedo, rf0, surface.roughness, diffuseFactor, specularFactor);
  diffuseFactor  = max(diffuseFactor, kMinDiffuseDemodulationFactor);
  specularFactor = max(specularFactor, kMinSpecularDemodulationFactor);
}

void WriteInvalidDenoiserSignals(int2 pixelPosition)
{
  // Invalid pixels still need deterministic denoiser inputs.
  motionVectorsImage[pixelPosition]               = float4(0.0, 0.0, 0.0, 0.0);
  normalRoughnessImage[pixelPosition]             = float4(0.0, 0.0, 0.0, 0.0);
  baseColorMetalnessImage[pixelPosition]          = float4(0.0, 0.0, 0.0, 0.0);
  viewZImage[pixelPosition]                       = kInvalidDenoiserViewZ;
  diffuseRadianceHitDistanceImage[pixelPosition]  = float4(0.0, 0.0, 0.0, 0.0);
  specularRadianceHitDistanceImage[pixelPosition] = float4(0.0, 0.0, 0.0, 0.0);
  specularDemodulationFactorImage[pixelPosition]  = float4(1.0, 1.0, 1.0, 1.0);
}

void WriteDenoiserSignals(int2 pixelPosition,
                          DISurface surface,
                          float3 diffuseRadiance,
                          float diffuseHitDistance,
                          float3 specularRadiance,
                          float specularHitDistance)
{
  if(!DIIsSurfaceValid(surface))
  {
    WriteInvalidDenoiserSignals(pixelPosition);
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const float viewZ = mul(float4(surface.worldPosition, 1.0), sceneInfo.viewMatrix).z;
  // Motion vectors tell NRD how to find this surface in the previous frame.
  const float3 motion = ComputeDenoiserMotionVector(float2(pixelPosition) + 0.5, surface.worldPosition, sceneInfo);
  motionVectorsImage[pixelPosition]   = float4(motion, 0.0);
  normalRoughnessImage[pixelPosition] = PackNrdNormalRoughness(surface.shadingNormal, surface.roughness);
  baseColorMetalnessImage[pixelPosition] = PackBaseColorMetalness(surface.albedo, surface.metallic);
  viewZImage[pixelPosition]           = viewZ;
  float3 diffuseDemodulationFactor;
  float3 specularDemodulationFactor;
  ComputeDenoiserMaterialFactors(surface, diffuseDemodulationFactor, specularDemodulationFactor);
  specularDemodulationFactorImage[pixelPosition] = float4(specularDemodulationFactor, 1.0);
  // Both lobes are demodulated before denoising, then remodulated in NrdCompose.
  diffuseRadianceHitDistanceImage[pixelPosition] =
      PackReblurRadianceHitDistance(max(diffuseRadiance, float3(0.0)) / diffuseDemodulationFactor, diffuseHitDistance, viewZ,
                                    surface.roughness, true);
  specularRadianceHitDistanceImage[pixelPosition] =
      PackReblurRadianceHitDistance(max(specularRadiance, float3(0.0)) / specularDemodulationFactor,
                                    specularHitDistance, viewZ, surface.roughness, false);
}

bool IsDepthDisoccluded(uint2 pixelPosition, DISurface surface)
{
  // The overlay marks pixels whose reprojected previous depth no longer matches.
  if(diParams.runtimeParams.frameIndex == 0u || !DIIsSurfaceValid(surface))
  {
    return false;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const float2 pixelCenter = float2(pixelPosition) + 0.5;

  float3 temporalMotionVector = float3(0.0);
  if(!TryComputeReSTIRTemporalMotionVector(pixelCenter, surface.worldPosition, surface.linearDepth, sceneInfo, temporalMotionVector))
  {
    return true;
  }

  // Screen-space motion points from the current pixel back to where this surface was last frame.
  const float3 screenSpaceMotion = ConvertMotionVectorToPixelSpace(pixelCenter, temporalMotionVector, sceneInfo);
  const int2 previousPixelPosition = int2(round(float2(pixelPosition) + screenSpaceMotion.xy));
  const DISurface previousSurface = DILoadGBufferSurface(previousPixelPosition, true);
  if(!DIIsSurfaceValid(previousSurface))
  {
    return true;
  }

  // Depth is compared at the reprojected previous pixel to detect newly revealed surfaces.
  const float expectedPreviousDepth = DIGetSurfaceLinearDepth(surface) + screenSpaceMotion.z;
  const float previousDepth = DIGetSurfaceLinearDepth(previousSurface);
  return !CompareRelativeDifference(expectedPreviousDepth, previousDepth, diParams.temporalResampling.depthThreshold);
}

float3 OverlayDepthDisocclusion(uint2 pixelPosition, DISurface surface, float3 fallbackColor)
{
  if(pushConst.debugView != uint(ReSTIRDebugView::eReSTIRDebugViewDepthDisocclusion))
  {
    return fallbackColor;
  }

  if(!IsDepthDisoccluded(pixelPosition, surface))
  {
    return fallbackColor;
  }

  return lerp(fallbackColor, float3(1.0, 0.04, 0.02), 0.30);
}

bool BeginSecondaryPathRay(SurfaceData surface, inout PathPayload payload, out RayDesc bounceRay)
{
  // Secondary path bounces add reflections/indirect transport after ReSTIR DI.
  bounceRay = (RayDesc)0;
  if(pushConst.secondaryPathMaxBounces == 0)
  {
    return false;
  }

  const float3 viewDir = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surface.worldPosition);
  bool    isTransmissionEvent = false;
  float3 bounceDir;
  // BSDF-over-PDF is the path throughput multiplier for the sampled secondary ray.
  float3 bsdfOverPdf        = float3(0.0);
  // The path tracer uses this PDF later if the secondary path performs direct-light MIS.
  float  directLightBsdfPdf = 0.0;
  // NRD needs to know whether the first secondary bounce was diffuse-like or specular-like.
  uint   sampledLobeKind    = 0u;
  if(!SampleSurfaceBsdf(surface, viewDir, payload.seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
  {
    return false;
  }

  // Reject near-tangent directions where the ray side and offset direction become numerically fragile.
  const float geometricDot = dot(surface.geometricNormal, bounceDir);
  if(abs(geometricDot) <= 1.0e-6)
  {
    return false;
  }

  // Throughput carries how much energy the path still represents after sampling this BSDF event.
  const float3 nextThroughput = payload.throughput * max(bsdfOverPdf, float3(0.0));
  if(SafeMax3(nextThroughput) <= 0.0)
  {
    return false;
  }

  // The continuation payload starts after the direct-light reservoir contribution.
  BeginFirstBounceDenoiserSignal(payload, sampledLobeKind);
  PrepareContinuationPayloadForBounce(payload, surface, bounceDir, nextThroughput, directLightBsdfPdf, isTransmissionEvent);

  bounceRay.Origin    = OffsetRay(surface.worldPosition, SelectOffsetNormal(surface.geometricNormal, bounceDir));
  bounceRay.Direction = bounceDir;
  bounceRay.TMin      = 0.001;
  bounceRay.TMax      = kRayTMax;
  return true;
}

bool TraceSecondaryPath(SurfaceData surface, inout PathPayload payload)
{
  // The final shading pass uses ray tracing only when a secondary bounce is requested.
  RayDesc bounceRay;
  if(!BeginSecondaryPathRay(surface, payload, bounceRay))
  {
    return false;
  }

  payload.depth = 1;
  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
  return true;
}

void ContinueHybridDIPath(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  // Closest-hit for secondary bounces delegates back to the shared path tracer continuation.
  const SurfaceData   surface   = LoadSurfaceData(attr);
  const float3        viewDir   = -WorldRayDirection();
  StoreFirstBounceHitDistance(payload, RayTCurrent());
  ContinuePathFromSurface(payload, surface, viewDir, pushConst.secondaryPathMaxBounces, true);
}

[shader("raygeneration")]
void rgenMain()
{
  // Final shading resolves the selected ReSTIR reservoir, optionally follows a
  // secondary path for reflections/indirect transport, writes NRD signals, and
  // finally applies debug views plus accumulation.
  const uint2 pixelPosition = DispatchRaysIndex().xy;
  const uint2 viewportSize  = uint2(pushConst.sceneInfoAddress[0].viewportSize);
  if(any(pixelPosition >= viewportSize))
  {
    return;
  }

  const uint pixelIndex = pixelPosition.y * viewportSize.x + pixelPosition.x;
  const DISurface surface = currentSurfaceBuffer[pixelIndex];
  const ReSTIRDIReservoir reservoir = LoadDIReservoir(diParams.reservoirBufferParams,
                                                      PixelPosToReservoirPos(pixelPosition),
                                                      diParams.bufferIndices.shadingInputBufferIndex);

  const DIResolvedDirectLighting directLighting = ResolveReservoirDirectLightingSplit(pixelPosition, surface, reservoir, true);
  // Direct lighting is the ReSTIR-controlled part of this hybrid path.
  float3 resolvedRadiance         = directLighting.totalRadiance;
  float3 resolvedDiffuseRadiance  = directLighting.diffuseRadiance;
  float  diffuseHitDistance       = directLighting.diffuseHitDistance;
  float3 resolvedSpecularRadiance = directLighting.specularRadiance;
  float  specularHitDistance      = directLighting.specularHitDistance;

  if(DIIsSurfaceValid(surface) && pushConst.secondaryPathMaxBounces > 0)
  {
    // Secondary bounces are accumulated on top of the already resolved direct-light estimate.
    const float3 preSecondaryPathRadiance = resolvedRadiance;
    PathPayload payload;
    // Start a small path-tracing payload whose first segment begins at the primary surface.
    payload                          = (PathPayload)0;
    payload.radiance                 = resolvedRadiance;
    payload.throughput               = float3(1.0);
    payload.mediumAttenuation        = float3(0.0);
    payload.mediumThickness          = 0.0;
    payload.lastSurfacePosition      = surface.worldPosition;
    payload.lastSurfaceNormal        = surface.shadingNormal;
    payload.lastDirectLightBsdfPdf   = 0.0;
    payload.seed                     = xxhash32(uint3(pixelPosition, diParams.runtimeParams.frameIndex));
    payload.depth                    = 0;
    payload.mediumActive             = 0;
    payload.lastEnvironmentNeeActive = 0;
    payload.hasLastBsdfSample        = 0;
    payload.primaryWorldPosition     = surface.worldPosition;
    payload.primaryRoughness         = surface.roughness;
    payload.primaryShadingNormal     = surface.shadingNormal;
    payload.hasPrimarySurface        = 1u;

    const SurfaceData unpackedSurface = UnpackDIPrimarySurface(surface);
    const bool tracedSecondaryPath = TraceSecondaryPath(unpackedSurface, payload);
    if(tracedSecondaryPath)
    {
      // A miss after the first secondary bounce still gives NRD a useful far hit distance.
      if(payload.awaitingFirstBounceHitDistance != 0u)
      {
        StoreFirstBounceHitDistance(payload, kDenoiserMissHitDistance);
      }

      // Isolate only the energy added by the secondary path, so NRD signal splits stay meaningful.
      const float3 secondaryPathRadiance = max(payload.radiance - preSecondaryPathRadiance, float3(0.0));
      if(payload.firstBounceIsSpecular == 0u)
      {
        const float previousDiffuseLuminance = ReSTIRLuminance(resolvedDiffuseRadiance);
        resolvedDiffuseRadiance += secondaryPathRadiance;
        if(SafeMax3(secondaryPathRadiance) > 0.0)
        {
          // NRD expects one hit distance per noisy signal. When a secondary path
          // contributes most of the energy, use its first useful hit distance.
          const float secondaryPathDiffuseHitDistance =
              (payload.hasDiffuseDenoiserHitDistance != 0u) ? payload.diffuseDenoiserHitDistance : diffuseHitDistance;
          if(diffuseHitDistance <= 0.0 || ReSTIRLuminance(secondaryPathRadiance) >= previousDiffuseLuminance)
          {
            diffuseHitDistance = secondaryPathDiffuseHitDistance;
          }
        }
      }
      else
      {
        const float previousSpecularLuminance = ReSTIRLuminance(resolvedSpecularRadiance);
        resolvedSpecularRadiance += secondaryPathRadiance;
        if(SafeMax3(secondaryPathRadiance) > 0.0)
        {
          // A specular first bounce that misses still gives NRD a large hit
          // distance, which behaves better than pretending the signal is local.
          const float secondaryPathSpecularHitDistance =
              (payload.hasSpecularDenoiserHitDistance != 0u) ? payload.specularDenoiserHitDistance : kDenoiserMissHitDistance;
          if(specularHitDistance <= 0.0 || ReSTIRLuminance(secondaryPathRadiance) >= previousSpecularLuminance)
          {
            specularHitDistance = secondaryPathSpecularHitDistance;
          }
        }
      }
    }
    resolvedRadiance = payload.radiance;
  }

  if((pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagWriteDenoiserSignals)) != 0)
  {
    // NRD consumes split diffuse/specular signals instead of the final combined image.
    WriteDenoiserSignals(int2(pixelPosition), surface, resolvedDiffuseRadiance, diffuseHitDistance, resolvedSpecularRadiance,
                         specularHitDistance);
  }

  resolvedRadiance = SanitizeRadiance(resolvedRadiance);
  // Debug overlays are applied after shading so they never alter reservoir contents.
  resolvedRadiance = VisualizeReSTIRDIDebug(pixelIndex, resolvedRadiance);
  resolvedRadiance = OverlayDepthDisocclusion(pixelPosition, surface, resolvedRadiance);
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDisabled)
     && (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagAccumulate)) != 0 && pushConst.accumulatedFrames > 0)
  {
    // Accumulation is a presentation/reference aid, not part of reservoir reuse.
    const float3 previousAverage = accumulationImage[int2(pixelPosition)].xyz;
    const float historyWeight    = float(pushConst.accumulatedFrames);
    resolvedRadiance = (previousAverage * historyWeight + resolvedRadiance) / (historyWeight + 1.0);
  }

  accumulationImage[int2(pixelPosition)] = float4(resolvedRadiance, 1.0);
  outImage[int2(pixelPosition)]          = float4(resolvedRadiance, 1.0);
}

[shader("miss")]
void rmissMain(inout PathPayload payload)
{
  AccumulateContinuationMissRadiance(payload);
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload)
{
  payload.visible = 1;
}

[shader("anyhit")]
void rahitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  if(IsMaskedSurfaceHit(attr))
  {
    IgnoreHit();
  }
}

[shader("anyhit")]
void shadowAnyHitMain(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  if(IsMaskedSurfaceHit(attr))
  {
    IgnoreHit();
  }
}

[shader("closesthit")]
void rchitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  ContinueHybridDIPath(payload, attr);
}
