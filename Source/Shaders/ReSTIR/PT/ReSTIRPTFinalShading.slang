#include <nvshaders/slang_types.h>
#include "nvshaders/random.h.slang"
#include "nvshaders/sky_functions.h.slang"
#include "ShaderIncludes/Pbr.h.slang"
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIo.h"
#include "ShaderIncludes/ReSTIR/PT/Globals.h.slang"

#define PATH_TRACING_CUSTOM_GLOBALS
#define PATH_TRACING_PAYLOAD_EXTRA_FIELDS \
  float3 firstBounceHitPosition; \
  float firstBounceHitDistance; \
  float3 firstBounceHitNormal; \
  uint firstBounceHitValid;
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Volume.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/PathTracing/Lights.h.slang"
#include "ShaderIncludes/ReSTIR/PT/PathReplay.h.slang"
#include "ShaderIncludes/ReSTIR/PT/Debug.h.slang"

[shader("raygeneration")]
void rgenMain()
{
  const uint2         launchID    = DispatchRaysIndex().xy;
  const GltfSceneInfo sceneInfo   = pushConst.sceneInfoAddress[0];
  const uint          pixelIndex  = FlattenPixelIndex(launchID, uint2(sceneInfo.viewportSize));
  const float2        pixelCenter = float2(launchID) + 0.5;

  float3 resolvedRadiance = float3(0.0);

  const ReSTIRPTPrimarySurface primarySurface = currentSurfaceBuffer[pixelIndex];
  if(IsValidPrimarySurface(primarySurface))
  {
    // Base radiance already contains the primary-hit direct lighting. Final
    // replay only reconstructs the selected continuation.
    resolvedRadiance = primarySurface.baseRadiance;

    const ReSTIRPTReservoir reservoir = currentHistoryReservoirBuffer[pixelIndex];
    if(IsValidReservoir(reservoir))
    {
      float3 worldDirection = float3(0.0);
      PathPayload payload;
      if(InitializeReplayPayload(payload, primarySurface, reservoir, worldDirection))
      {
        const float3 originNormal = SelectOffsetNormal(primarySurface.geometricNormal, worldDirection);

        RayDesc bounceRay;
        bounceRay.Origin    = OffsetRay(primarySurface.worldPosition, originNormal);
        bounceRay.Direction = worldDirection;
        bounceRay.TMin      = 0.001;
        bounceRay.TMax      = kRayTMax;

        TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, bounceRay, payload);
        // The replay payload starts from base radiance, so only the suffix is
        // scaled by the final reservoir weight here.
        const float3 replayedContinuation = max(payload.radiance - primarySurface.baseRadiance, float3(0.0));
        resolvedRadiance = primarySurface.baseRadiance + replayedContinuation * reservoir.weight;
      }
    }
  }
  else
  {
    const float3 primaryDirection = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
    resolvedRadiance              = SampleEnvironment(sceneInfo, primaryDirection);
  }

  resolvedRadiance = SanitizeRadiance(resolvedRadiance);
  resolvedRadiance = VisualizeReSTIRPTDebug(pixelIndex, resolvedRadiance);
  if(pushConst.debugView == uint(ReSTIRDebugView::eReSTIRDebugViewDisabled) && IsReSTIRPTAccumulationEnabled()
     && pushConst.accumulatedFrames > 0)
  {
    const float3 previousAverage = accumulationImage[int2(launchID)].xyz;
    const float  historyWeight   = float(pushConst.accumulatedFrames);
    resolvedRadiance = (previousAverage * historyWeight + resolvedRadiance) / (historyWeight + 1.0);
  }

  accumulationImage[int2(launchID)] = float4(resolvedRadiance, 1.0);
  outImage[int2(launchID)]          = float4(resolvedRadiance, 1.0);
}

[shader("miss")]
void rmissMain(inout PathPayload payload)
{
  AccumulateReSTIRPTMissRadiance(payload);
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
  ContinueReSTIRPTPath(payload, attr);
}
