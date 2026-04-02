#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "NRD.hlsli"
#include "ShaderIncludes/ReSTIR/GI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/GI/Debug.h.slang"
#include "ShaderIncludes/PathTracing/Continuation.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"

static const float kInvalidDenoiserViewZ      = 1.0e32;
static const float kDenoiserMissHitDistance   = 65504.0;
static const float4 kReblurHitDistanceParameters = float4(3.0, 0.1, 20.0, -25.0);
static const float3 kMinDiffuseDemodulationFactor = float3(0.05, 0.05, 0.05);

float4 PackNrdNormalRoughness(float3 shadingNormal, float roughness)
{
  return NRD_FrontEnd_PackNormalAndRoughness(shadingNormal, roughness, 0.0);
}

float4 PackReblurRadianceHitDistance(float3 radiance, float hitDistance, float viewZ, float roughness, bool isDiffuse)
{
  const float normalizedHitDistance =
      REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, kReblurHitDistanceParameters, isDiffuse ? 1.0 : roughness);
  return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normalizedHitDistance, true);
}

float4 PackBaseColorMetalness(float3 baseColor, float metalness)
{
  return float4(saturate(baseColor), saturate(metalness));
}

float3 ComputeDiffuseDemodulationFactor(GISurface surface)
{
  const float3 diffuseFactor = surface.albedo * (1.0 - surface.metallic);
  return max(diffuseFactor, kMinDiffuseDemodulationFactor);
}

float GIGetReservoirHitDistance(GISurface surface, ReSTIRGIReservoir reservoir)
{
  if(!GIIsSurfaceValid(surface) || !IsValidGIReservoir(reservoir))
  {
    return 0.0;
  }

  if(GIIsEnvironmentSample(reservoir))
  {
    return kDenoiserMissHitDistance;
  }

  return length(reservoir.samplePosition - surface.worldPosition);
}

void WriteInvalidDenoiserSignals(int2 pixelPosition)
{
  motionVectorsImage[pixelPosition]               = float4(0.0, 0.0, 0.0, 0.0);
  normalRoughnessImage[pixelPosition]             = float4(0.0, 0.0, 0.0, 0.0);
  baseColorMetalnessImage[pixelPosition]          = float4(0.0, 0.0, 0.0, 0.0);
  viewZImage[pixelPosition]                       = kInvalidDenoiserViewZ;
  diffuseRadianceHitDistanceImage[pixelPosition]  = float4(0.0, 0.0, 0.0, 0.0);
  specularRadianceHitDistanceImage[pixelPosition] = float4(0.0, 0.0, 0.0, 0.0);
}

void WriteDenoiserSignals(int2 pixelPosition,
                          GISurface surface,
                          float3 diffuseRadiance,
                          float diffuseHitDistance,
                          float3 specularRadiance,
                          float specularHitDistance)
{
  if(!GIIsSurfaceValid(surface))
  {
    WriteInvalidDenoiserSignals(pixelPosition);
    return;
  }

  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const float viewZ = mul(float4(surface.worldPosition, 1.0), sceneInfo.viewMatrix).z;
  const float3 motion = ComputeDenoiserMotionVector(float2(pixelPosition) + 0.5, surface.worldPosition, sceneInfo);
  motionVectorsImage[pixelPosition]   = float4(motion, 0.0);
  normalRoughnessImage[pixelPosition] = PackNrdNormalRoughness(surface.shadingNormal, surface.roughness);
  baseColorMetalnessImage[pixelPosition] = PackBaseColorMetalness(surface.albedo, surface.metallic);
  viewZImage[pixelPosition]           = viewZ;
  const float3 diffuseDemodulationFactor = ComputeDiffuseDemodulationFactor(surface);
  diffuseRadianceHitDistanceImage[pixelPosition] =
      PackReblurRadianceHitDistance(max(diffuseRadiance, float3(0.0)) / diffuseDemodulationFactor, diffuseHitDistance, viewZ,
                                    surface.roughness, true);
  specularRadianceHitDistanceImage[pixelPosition] =
      PackReblurRadianceHitDistance(max(specularRadiance, float3(0.0)), specularHitDistance, viewZ, surface.roughness, false);
}

bool BeginGlossyGIContinuationRay(inout PathPayload payload, SurfaceData surface, float3 viewDir, out RayDesc bounceRay)
{
  bounceRay = (RayDesc)0;

  float3 bounceDir           = float3(0.0);
  float3 bsdfOverPdf         = float3(0.0);
  float  directLightBsdfPdf  = 0.0;
  bool   isTransmissionEvent = false;
  uint   sampledLobeKind     = 0u;
  if(!SampleSurfaceBsdf(surface, viewDir, payload.seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
  {
    return false;
  }

  // The GI reservoir already carries the broad first-bounce indirect term.
  // The hybrid continuation only owns glossy / glass / clearcoat transport.
  if(sampledLobeKind == 0u)
  {
    return false;
  }

  const float geometricDot = dot(surface.geometricNormal, bounceDir);
  if(abs(geometricDot) <= 1.0e-6)
  {
    return false;
  }

  const float3 nextThroughput = payload.throughput * max(bsdfOverPdf, float3(0.0));
  if(SafeMax3(nextThroughput) <= 0.0)
  {
    return false;
  }

  BeginFirstBounceDenoiserSignal(payload, sampledLobeKind);
  PrepareContinuationPayloadForBounce(payload, surface, bounceDir, nextThroughput, directLightBsdfPdf, isTransmissionEvent);

  bounceRay.Origin    = OffsetRay(surface.worldPosition, SelectOffsetNormal(surface.geometricNormal, bounceDir));
  bounceRay.Direction = bounceDir;
  bounceRay.TMin      = 0.001;
  bounceRay.TMax      = kRayTMax;
  return true;
}

bool TraceHybridGIContinuation(GISurface primarySurface, inout PathPayload payload)
{
  if(pushConst.continuationMaxBounces == 0u || !GIIsSurfaceValid(primarySurface))
  {
    return false;
  }

  const SurfaceData surface = UnpackGIPrimarySurface(primarySurface);
  const float3      viewDir = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surface.worldPosition);

  RayDesc bounceRay;
  if(!BeginGlossyGIContinuationRay(payload, surface, viewDir, bounceRay))
  {
    return false;
  }

  payload.depth = 1u;
  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
  return true;
}

void ContinueHybridGIPath(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  const SurfaceData surface = LoadSurfaceData(attr);
  const float3      viewDir = -WorldRayDirection();
  StoreFirstBounceHitDistance(payload, RayTCurrent());
  ContinuePathFromSurface(payload, surface, viewDir, pushConst.continuationMaxBounces, true);
}

[shader("raygeneration")]
void rgenMain()
{
  const uint2 pixelPosition = DispatchRaysIndex().xy;
  const uint2 viewportSize  = uint2(pushConst.sceneInfoAddress[0].viewportSize);
  if(any(pixelPosition >= viewportSize))
  {
    return;
  }

  const uint pixelIndex = pixelPosition.y * viewportSize.x + pixelPosition.x;
  const GISurface surface = currentSurfaceBuffer[pixelIndex];
  const ReSTIRGIReservoir reservoir =
      LoadGIReservoir(giParams.reservoirBufferParams, PixelPosToReservoirPos(pixelPosition, giParams.runtimeParams.activeCheckerboardField),
                      giParams.bufferIndices.shadingInputBufferIndex);
  const ReSTIRGIReservoir initialReservoir = giInitialSampleBuffer[pixelIndex];

  float3 resolvedRadiance = GIIsSurfaceValid(surface) ? surface.baseRadiance : ReconstructGIMissRadiance(pixelPosition);
  float3 resolvedDiffuseRadiance = GIIsSurfaceValid(surface) ? surface.baseRadiance : float3(0.0);
  float diffuseHitDistance = 0.0;
  float3 resolvedSpecularRadiance = float3(0.0);
  float specularHitDistance = 0.0;
  if(GIIsSurfaceValid(surface))
  {
    float3 reusedContribution = float3(0.0);
    float3 reusedBsdf         = float3(0.0);
    const bool hasReusedContribution = GIEvaluateReservoirContribution(surface, reservoir, true, reusedContribution, reusedBsdf);

    float3 initialContribution = float3(0.0);
    float3 initialBsdf         = float3(0.0);
    const bool hasInitialContribution = GIEvaluateReservoirContribution(surface, initialReservoir, false, initialContribution, initialBsdf);

    if(giParams.shading.enableFinalMIS != 0u && hasReusedContribution && hasInitialContribution)
    {
      const GISurface roughenedSurface = GIMakeFinalMISRoughenedSurface(surface);

      float roughReusedPdf = 0.0;
      float roughInitialPdf = 0.0;
      const float3 roughReusedBsdf = GIEvaluateReservoirBsdf(roughenedSurface, reservoir, roughReusedPdf);
      const float3 roughInitialBsdf = GIEvaluateReservoirBsdf(roughenedSurface, initialReservoir, roughInitialPdf);

      const float reusedWeight  = 1.0 - GIGetFinalMISWeight(roughReusedBsdf, reusedBsdf);
      const float initialWeight = GIGetFinalMISWeight(roughInitialBsdf, initialBsdf);
      const float3 blendedContribution = reusedContribution * reusedWeight + initialContribution * initialWeight;
      resolvedRadiance += blendedContribution;
      resolvedDiffuseRadiance += blendedContribution;
      diffuseHitDistance = (ReSTIRLuminance(reusedContribution * reusedWeight) >= ReSTIRLuminance(initialContribution * initialWeight))
                               ? GIGetReservoirHitDistance(surface, reservoir)
                               : GIGetReservoirHitDistance(surface, initialReservoir);
    }
    else if(hasReusedContribution)
    {
      resolvedRadiance += reusedContribution;
      resolvedDiffuseRadiance += reusedContribution;
      diffuseHitDistance = GIGetReservoirHitDistance(surface, reservoir);
    }
    else if(hasInitialContribution)
    {
      resolvedRadiance += initialContribution;
      resolvedDiffuseRadiance += initialContribution;
      diffuseHitDistance = GIGetReservoirHitDistance(surface, initialReservoir);
    }
  }

  if(GIIsSurfaceValid(surface) && pushConst.continuationMaxBounces > 0u)
  {
    const float3 preContinuationRadiance = resolvedRadiance;
    PathPayload payload;
    payload                          = (PathPayload)0;
    payload.radiance                 = resolvedRadiance;
    payload.throughput               = float3(1.0);
    payload.mediumAttenuation        = float3(0.0);
    payload.mediumThickness          = 0.0;
    payload.lastSurfacePosition      = surface.worldPosition;
    payload.lastSurfaceNormal        = surface.shadingNormal;
    payload.lastDirectLightBsdfPdf   = 0.0;
    payload.seed                     = xxhash32(uint3(pixelPosition, giParams.runtimeParams.frameIndex));
    payload.depth                    = 0u;
    payload.mediumActive             = 0u;
    payload.lastEnvironmentNeeActive = 0u;
    payload.hasLastBsdfSample        = 0u;
    payload.primaryWorldPosition     = surface.worldPosition;
    payload.primaryRoughness         = surface.roughness;
    payload.primaryShadingNormal     = surface.shadingNormal;
    payload.hasPrimarySurface        = 1u;
    const bool tracedContinuation = TraceHybridGIContinuation(surface, payload);
    if(tracedContinuation)
    {
      if(payload.awaitingFirstBounceHitDistance != 0u)
      {
        StoreFirstBounceHitDistance(payload, kDenoiserMissHitDistance);
      }

      resolvedSpecularRadiance = max(payload.radiance - preContinuationRadiance, float3(0.0));
      if(SafeMax3(resolvedSpecularRadiance) > 0.0)
      {
        specularHitDistance = (payload.hasSpecularDenoiserHitDistance != 0u) ? payload.specularDenoiserHitDistance : kDenoiserMissHitDistance;
      }
    }
    resolvedRadiance = payload.radiance;
  }

  WriteDenoiserSignals(int2(pixelPosition), surface, resolvedDiffuseRadiance, diffuseHitDistance, resolvedSpecularRadiance,
                       specularHitDistance);

  resolvedRadiance = SanitizeRadiance(resolvedRadiance);
  resolvedRadiance = VisualizeReSTIRGIDebug(pixelIndex, resolvedRadiance);
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDisabled)
     && (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagAccumulate)) != 0u && pushConst.accumulatedFrames > 0u)
  {
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
  payload.visible = 1u;
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
  ContinueHybridGIPath(payload, attr);
}
