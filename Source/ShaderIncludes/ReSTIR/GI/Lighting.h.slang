#ifndef RESTIR_GI_LIGHTING_H
#define RESTIR_GI_LIGHTING_H

#define RESTIR_GI_RESERVOIR_BUFFER giReservoirBuffer

#ifndef RESTIR_GI_ALLOWED_BIAS_CORRECTION
#define RESTIR_GI_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_RAY_TRACED
#endif

#ifndef RESTIR_GI_ENABLE_FINAL_VISIBILITY
#define RESTIR_GI_ENABLE_FINAL_VISIBILITY 1
#endif

#include "ShaderIncludes/ReSTIR/Common/Common.h.slang"
#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ReSTIR/Common/Utils/Math.hlsli"
#include "ReSTIR/Common/Utils/Checkerboard.hlsli"
#include "ReSTIR/GI/Reservoir.hlsli"

#if RESTIR_GI_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_RAY_TRACED || RESTIR_GI_ENABLE_FINAL_VISIBILITY
#include "ShaderIncludes/PathTracing/Intersection.h.slang"
#endif

typedef ReSTIRGIPrimarySurface GISurface;
typedef uint                   GIMaterial;

static const float kGIFinalMISMaxBsdfValue = 1.0e4;
static const float kGIFinalMISRoughness    = 0.3;

float3 GIEvaluateReservoirBsdf(GISurface surface, ReSTIRGIReservoir reservoir, out float samplePdf);
GISurface GIMakeFinalMISRoughenedSurface(GISurface surface);
float GIGetFinalMISWeight(float3 roughBsdf, float3 trueBsdf);
bool GIEvaluateReservoirContribution(GISurface surface, ReSTIRGIReservoir reservoir, bool applyFinalVisibility, out float3 contribution,
                                     out float3 bsdf);

GISurface EmptyGIPrimarySurface()
{
  return (GISurface)0;
}

bool GIIsSurfaceValid(GISurface surface)
{
  return surface.valid != 0u;
}

GISurface PackGIPrimarySurface(SurfaceData surface, uint materialIndex, float linearDepth, float3 baseRadiance)
{
  GISurface packed           = EmptyGIPrimarySurface();
  packed.worldPosition       = surface.worldPosition;
  packed.linearDepth         = linearDepth;
  packed.shadingNormal       = surface.shadingNormal;
  packed.roughness           = surface.roughness;
  packed.geometricNormal     = surface.geometricNormal;
  packed.metallic            = surface.metallic;
  packed.tangent             = surface.tangent;
  packed.specular            = surface.specular;
  packed.bitangent           = surface.bitangent;
  packed.specularTint        = surface.specularTint;
  packed.albedo              = surface.albedo;
  packed.transmission        = surface.transmission;
  packed.emission            = surface.emission;
  packed.attenuationDistance = surface.attenuationDistance;
  packed.attenuationColor    = surface.attenuationColor;
  packed.volumeThickness     = surface.volumeThickness;
  packed.sheenColor          = surface.sheenColor;
  packed.refractionIndex     = surface.refractionIndex;
  packed.clearcoat           = surface.clearcoat;
  packed.clearcoatRoughness  = surface.clearcoatRoughness;
  packed.sheenRoughness      = surface.sheenRoughness;
  packed.subsurface          = surface.subsurface;
  packed.anisotropy          = surface.anisotropy;
  packed.materialIndex       = materialIndex;
  packed.isFrontFace         = surface.isFrontFace;
  packed.valid               = 1u;
  packed.baseRadiance        = SanitizeRadiance(baseRadiance);
  return packed;
}

SurfaceData UnpackGIPrimarySurface(GISurface surface)
{
  SurfaceData unpacked;
  unpacked.worldPosition       = surface.worldPosition;
  unpacked.shadingNormal       = surface.shadingNormal;
  unpacked.geometricNormal     = surface.geometricNormal;
  unpacked.tangent             = surface.tangent;
  unpacked.bitangent           = surface.bitangent;
  unpacked.texCoord            = float2(0.0);
  unpacked.albedo              = surface.albedo;
  unpacked.emission            = surface.emission;
  unpacked.metallic            = surface.metallic;
  unpacked.roughness           = surface.roughness;
  unpacked.specular            = surface.specular;
  unpacked.specularTint        = surface.specularTint;
  unpacked.subsurface          = surface.subsurface;
  unpacked.anisotropy          = surface.anisotropy;
  unpacked.attenuationColor    = surface.attenuationColor;
  unpacked.transmission        = surface.transmission;
  unpacked.attenuationDistance = surface.attenuationDistance;
  unpacked.volumeThickness     = surface.volumeThickness;
  unpacked.refractionIndex     = surface.refractionIndex;
  unpacked.clearcoat           = surface.clearcoat;
  unpacked.clearcoatRoughness  = surface.clearcoatRoughness;
  unpacked.sheenColor          = surface.sheenColor;
  unpacked.sheenRoughness      = surface.sheenRoughness;
  unpacked.isFrontFace         = surface.isFrontFace;
  return unpacked;
}

float3 GIGetSurfaceNormal(GISurface surface)
{
  return surface.shadingNormal;
}

float GIGetSurfaceLinearDepth(GISurface surface)
{
  return surface.linearDepth;
}

float3 GIGetSurfaceWorldPosition(GISurface surface)
{
  return surface.worldPosition;
}

GIMaterial GIGetMaterial(GISurface surface)
{
  return surface.materialIndex;
}

bool GIAreMaterialsSimilar(GIMaterial a, GIMaterial b)
{
  return a == b;
}

bool GIIsEnvironmentSample(ReSTIRGIReservoir reservoir)
{
  return reservoir.sampleKind == uint(ReSTIRGISampleKind::eReSTIRGISampleKindEnvironment);
}

float3 GIGetReservoirIncidentDirection(GISurface surface, ReSTIRGIReservoir reservoir)
{
  if(GIIsEnvironmentSample(reservoir))
  {
    return normalize(reservoir.sampleDirection);
  }

  const float3 toSample = reservoir.samplePosition - surface.worldPosition;
  if(dot(toSample, toSample) <= 1.0e-8)
  {
    return float3(0.0);
  }

  return normalize(toSample);
}

float GICalculateJacobianForSurface(GISurface receiverSurface, GISurface sourceSurface, ReSTIRGIReservoir reservoir)
{
  if(GIIsEnvironmentSample(reservoir))
  {
    return 1.0;
  }

  return CalculateJacobian(GIGetSurfaceWorldPosition(receiverSurface), GIGetSurfaceWorldPosition(sourceSurface), reservoir.samplePosition,
                           reservoir.sampleNormal);
}

int2 GIClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
  const int width  = int(pushConst.sceneInfoAddress[0].viewportSize.x);
  const int height = int(pushConst.sceneInfoAddress[0].viewportSize.y);
  if(width <= 0 || height <= 0)
  {
    return int2(0, 0);
  }

  if(pixelPosition.x < 0)
  {
    pixelPosition.x = -pixelPosition.x;
  }
  if(pixelPosition.y < 0)
  {
    pixelPosition.y = -pixelPosition.y;
  }
  if(pixelPosition.x >= width)
  {
    pixelPosition.x = 2 * width - pixelPosition.x - 1;
  }
  if(pixelPosition.y >= height)
  {
    pixelPosition.y = 2 * height - pixelPosition.y - 1;
  }

  return pixelPosition;
}

GISurface GILoadGBufferSurface(int2 pixelPosition, bool previousFrame)
{
  const int width  = int(pushConst.sceneInfoAddress[0].viewportSize.x);
  const int height = int(pushConst.sceneInfoAddress[0].viewportSize.y);
  if(pixelPosition.x < 0 || pixelPosition.y < 0 || pixelPosition.x >= width || pixelPosition.y >= height)
  {
    return EmptyGIPrimarySurface();
  }

  const uint pixelIndex = uint(pixelPosition.y) * uint(width) + uint(pixelPosition.x);
  return previousFrame ? previousSurfaceBuffer[pixelIndex] : currentSurfaceBuffer[pixelIndex];
}

float GIGetSampleTargetPdfForSurface(ReSTIRGIReservoir reservoir, GISurface surface)
{
  float  samplePdf = 0.0;
  const float3 brdf = GIEvaluateReservoirBsdf(surface, reservoir, samplePdf);
  return ReSTIRLuminance(max(brdf * reservoir.sampleRadiance, float3(0.0)));
}

bool GIValidateSampleWithJacobian(inout float jacobian)
{
  if(jacobian > 10.0 || jacobian < (1.0 / 10.0))
  {
    return false;
  }

  jacobian = clamp(jacobian, 1.0 / 3.0, 3.0);
  return true;
}

bool GIGetConservativeVisibility(GISurface surface, ReSTIRGIReservoir reservoir)
{
#if RESTIR_GI_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_RAY_TRACED || RESTIR_GI_ENABLE_FINAL_VISIBILITY
  if(GIIsEnvironmentSample(reservoir))
  {
    const float3 sampleDirection = normalize(reservoir.sampleDirection);
    if(dot(sampleDirection, sampleDirection) <= 1.0e-8)
    {
      return false;
    }

    return TraceVisibility(surface.worldPosition, surface.geometricNormal, sampleDirection);
  }

  const float3 toSample = reservoir.samplePosition - surface.worldPosition;
  const float distanceSquared = dot(toSample, toSample);
  if(distanceSquared <= 1.0e-8)
  {
    return false;
  }

  const float distance = sqrt(distanceSquared);
  return TraceVisibility(surface.worldPosition, surface.geometricNormal, toSample / distance, max(distance - 0.001, 0.001));
#else
  return true;
#endif
}

bool GIGetTemporalConservativeVisibility(GISurface currentSurface, GISurface previousSurface, ReSTIRGIReservoir reservoir)
{
#if RESTIR_GI_ALLOWED_BIAS_CORRECTION >= RESTIR_BIAS_CORRECTION_RAY_TRACED
  return GIGetConservativeVisibility(currentSurface, reservoir);
#else
  return true;
#endif
}

float3 ReconstructGIMissRadiance(uint2 pixelPosition)
{
  const GltfSceneInfo sceneInfo   = pushConst.sceneInfoAddress[0];
  const float2 pixelCenter        = float2(pixelPosition) + 0.5;
  const float3 worldDirection     = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
  return SampleEnvironment(sceneInfo, worldDirection);
}

float3 GIEvaluateReservoirBsdf(GISurface surface, ReSTIRGIReservoir reservoir, out float samplePdf)
{
  if(!GIIsSurfaceValid(surface))
  {
    samplePdf = 0.0;
    return float3(0.0);
  }

  const float3 incidentDirection = GIGetReservoirIncidentDirection(surface, reservoir);
  if(dot(incidentDirection, incidentDirection) <= 1.0e-8)
  {
    samplePdf = 0.0;
    return float3(0.0);
  }

  const SurfaceData unpackedSurface = UnpackGIPrimarySurface(surface);
  const float3      viewDirection   = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surface.worldPosition);

  // ReSTIR GI owns only the broad first-bounce indirect estimator. Glossy /
  // glass / clearcoat transport is handled by the hybrid continuation path.
  return EvaluateDirectLightBsdf(unpackedSurface, viewDirection, incidentDirection, samplePdf);
}

GISurface GIMakeFinalMISRoughenedSurface(GISurface surface)
{
  GISurface roughened = surface;
  roughened.roughness = max(roughened.roughness, kGIFinalMISRoughness);
  return roughened;
}

float GIGetFinalMISWeight(float3 roughBsdf, float3 trueBsdf)
{
  const float3 clampedRoughBsdf = clamp(roughBsdf, 1.0e-4, kGIFinalMISMaxBsdfValue);
  const float3 clampedTrueBsdf  = clamp(trueBsdf, 0.0, kGIFinalMISMaxBsdfValue);
  const float  weight           = saturate(ReSTIRLuminance(clampedTrueBsdf) / ReSTIRLuminance(clampedTrueBsdf + clampedRoughBsdf));
  return weight * weight * weight;
}

bool GIEvaluateReservoirContribution(GISurface surface, ReSTIRGIReservoir reservoir, bool applyFinalVisibility, out float3 contribution,
                                     out float3 bsdf)
{
  contribution = float3(0.0);
  bsdf         = float3(0.0);
  if(!IsValidGIReservoir(reservoir))
  {
    return false;
  }

  float samplePdf = 0.0;
  bsdf            = GIEvaluateReservoirBsdf(surface, reservoir, samplePdf);
  if(samplePdf <= 0.0 || SafeMax3(bsdf) <= 0.0)
  {
    return false;
  }

  float3 visibility = float3(1.0);
  if(applyFinalVisibility && giParams.shading.enableFinalVisibility != 0u)
  {
    visibility = GIGetConservativeVisibility(surface, reservoir) ? float3(1.0) : float3(0.0);
  }

  contribution = max(bsdf * reservoir.sampleRadiance, float3(0.0)) * visibility * reservoir.weightSum;
  return true;
}

float3 ResolveGIReservoirLighting(uint2 pixelPosition, GISurface surface, ReSTIRGIReservoir reservoir)
{
  if(!GIIsSurfaceValid(surface))
  {
    return ReconstructGIMissRadiance(pixelPosition);
  }

  float3 radiance = surface.baseRadiance;

  float3 contribution = float3(0.0);
  float3 bsdf         = float3(0.0);
  if(GIEvaluateReservoirContribution(surface, reservoir, true, contribution, bsdf))
  {
    radiance += contribution;
  }

  return SanitizeRadiance(radiance);
}

#endif
