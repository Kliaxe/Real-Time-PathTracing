#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "NRD.hlsli"
#include "ShaderIncludes/ReSTIR/DI/Globals.h.slang"
#include "ShaderIncludes/ReSTIR/Common/Common.h.slang"
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Continuation.h.slang"
#include "ShaderIncludes/ReSTIR/DI/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/DI/Debug.h.slang"

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

float3 ComputeDiffuseDemodulationFactor(DISurface surface)
{
  const float3 diffuseFactor = surface.albedo * (1.0 - surface.metallic);
  return max(diffuseFactor, kMinDiffuseDemodulationFactor);
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

bool BeginHybridDIContinuationRay(SurfaceData surface, inout PathPayload payload, out RayDesc bounceRay)
{
  bounceRay = (RayDesc)0;
  if(pushConst.continuationMaxBounces == 0)
  {
    return false;
  }

  const float3 viewDir = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surface.worldPosition);
  bool    isTransmissionEvent = false;
  float3 bounceDir;
  float3 bsdfOverPdf        = float3(0.0);
  float  directLightBsdfPdf = 0.0;
  uint   sampledLobeKind    = 0u;
  if(!SampleSurfaceBsdf(surface, viewDir, payload.seed, bounceDir, bsdfOverPdf, directLightBsdfPdf, isTransmissionEvent, sampledLobeKind))
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

bool TraceHybridDIContinuation(SurfaceData surface, inout PathPayload payload)
{
  RayDesc bounceRay;
  if(!BeginHybridDIContinuationRay(surface, payload, bounceRay))
  {
    return false;
  }

  payload.depth = 1;
  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
  return true;
}

void ContinueHybridDIPath(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  const SurfaceData   surface   = LoadSurfaceData(attr);
  const float3        viewDir   = -WorldRayDirection();
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
  const DISurface surface = currentSurfaceBuffer[pixelIndex];
  const ReSTIRDIReservoir reservoir = LoadDIReservoir(diParams.reservoirBufferParams,
                                                      PixelPosToReservoirPos(pixelPosition, diParams.runtimeParams.activeCheckerboardField),
                                                      diParams.bufferIndices.shadingInputBufferIndex);

  const DIResolvedDirectLighting directLighting = ResolveReservoirDirectLightingSplit(pixelPosition, surface, reservoir, true);
  float3 resolvedRadiance         = directLighting.totalRadiance;
  float3 resolvedDiffuseRadiance  = directLighting.diffuseRadiance;
  float  diffuseHitDistance       = directLighting.diffuseHitDistance;
  float3 resolvedSpecularRadiance = directLighting.specularRadiance;
  float  specularHitDistance      = directLighting.specularHitDistance;

  if(DIIsSurfaceValid(surface) && pushConst.continuationMaxBounces > 0)
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
    const bool tracedContinuation = TraceHybridDIContinuation(unpackedSurface, payload);
    if(tracedContinuation)
    {
      if(payload.awaitingFirstBounceHitDistance != 0u)
      {
        StoreFirstBounceHitDistance(payload, kDenoiserMissHitDistance);
      }

      const float3 continuationRadiance = max(payload.radiance - preContinuationRadiance, float3(0.0));
      if(payload.firstBounceIsSpecular == 0u)
      {
        const float previousDiffuseLuminance = ReSTIRLuminance(resolvedDiffuseRadiance);
        resolvedDiffuseRadiance += continuationRadiance;
        if(SafeMax3(continuationRadiance) > 0.0)
        {
          const float continuationDiffuseHitDistance =
              (payload.hasDiffuseDenoiserHitDistance != 0u) ? payload.diffuseDenoiserHitDistance : diffuseHitDistance;
          if(diffuseHitDistance <= 0.0 || ReSTIRLuminance(continuationRadiance) >= previousDiffuseLuminance)
          {
            diffuseHitDistance = continuationDiffuseHitDistance;
          }
        }
      }
      else
      {
        const float previousSpecularLuminance = ReSTIRLuminance(resolvedSpecularRadiance);
        resolvedSpecularRadiance += continuationRadiance;
        if(SafeMax3(continuationRadiance) > 0.0)
        {
          const float continuationSpecularHitDistance =
              (payload.hasSpecularDenoiserHitDistance != 0u) ? payload.specularDenoiserHitDistance : kDenoiserMissHitDistance;
          if(specularHitDistance <= 0.0 || ReSTIRLuminance(continuationRadiance) >= previousSpecularLuminance)
          {
            specularHitDistance = continuationSpecularHitDistance;
          }
        }
      }
    }
    resolvedRadiance = payload.radiance;
  }

  WriteDenoiserSignals(int2(pixelPosition), surface, resolvedDiffuseRadiance, diffuseHitDistance, resolvedSpecularRadiance,
                       specularHitDistance);

  resolvedRadiance = SanitizeRadiance(resolvedRadiance);
  resolvedRadiance = VisualizeReSTIRDIDebug(pixelIndex, resolvedRadiance);
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDisabled)
     && (pushConst.flags & uint(ReSTIRFlags::eReSTIRFlagAccumulate)) != 0 && pushConst.accumulatedFrames > 0)
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
