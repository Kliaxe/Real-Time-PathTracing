#include <nvshaders/slang_types.h>
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIncludes/ReSTIR/Globals.h.slang"
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#include "ShaderIncludes/ReSTIR/Lighting.h.slang"
#include "ShaderIncludes/ReSTIR/Debug.h.slang"
#include "ReSTIR/InitialSampling.hlsli"

struct DIPrimaryPayload
{
  ReSTIRDISurface surface;
  ReSTIRDIReservoir reservoir;
  uint2 pixelPosition;
};

[shader("raygeneration")]
void rgenMain()
{
  const uint2 launchID = DispatchRaysIndex().xy;
  const uint  width    = uint(pushConst.sceneInfoAddress[0].viewportSize.x);
  const uint  pixelIndex = launchID.y * width + launchID.x;

  RayDesc ray;
  ray.Origin    = pushConst.sceneInfoAddress[0].cameraPosition;
  ray.Direction = ReconstructWorldDirectionFromPixel(float2(launchID) + 0.5, pushConst.sceneInfoAddress[0]);
  ray.TMin      = 0.001;
  ray.TMax      = kRayTMax;

  DIPrimaryPayload payload;
  payload.surface       = EmptyDIPrimarySurface();
  payload.reservoir     = EmptyDIReservoir();
  payload.pixelPosition = launchID;

  TraceRay(topLevelAS, 0, 0xFF, 0, 0, 0, ray, payload);

  currentSurfaceBuffer[pixelIndex] = payload.surface;
  StoreDIReservoir(payload.reservoir, diParams.reservoirBufferParams,
                   PixelPosToReservoirPos(launchID),
                   diParams.bufferIndices.initialSamplingOutputBufferIndex);
  WriteDIDebugPixel(pixelIndex, payload.reservoir, 1.0, payload.reservoir.M, uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone),
                    uint(ReSTIRShiftStatus::eReSTIRShiftStatusNone));
}

[shader("miss")]
void rmissMain(inout DIPrimaryPayload payload)
{
  payload.surface   = EmptyDIPrimarySurface();
  payload.reservoir = EmptyDIReservoir();
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload)
{
  payload.visible = 1;
}

[shader("anyhit")]
void rahitMain(inout DIPrimaryPayload payload, in BuiltInTriangleIntersectionAttributes attr)
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
void rchitMain(inout DIPrimaryPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  const SurfaceData surface = LoadSurfaceData(attr);
  payload.surface = PackDIPrimarySurface(surface, pushConst.sceneInfoAddress[0].instances[InstanceIndex()].materialIndex, InstanceIndex(),
                                         PrimitiveIndex(), RayTCurrent());

  ReSTIRRandomSamplerState rng = InitRandomSampler(payload.pixelPosition, diParams.runtimeParams.frameIndex, 0u);
  DILightSample selectedSample;
  payload.reservoir = ReSTIRDISampleLightsForSurface(rng, payload.surface, diParams.initialSampling,
                                                     BuildLightBufferParameters(pushConst.sceneInfoAddress[0]), selectedSample);
}
