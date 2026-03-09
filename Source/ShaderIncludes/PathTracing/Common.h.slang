#ifndef PATH_TRACING_COMMON_H
#define PATH_TRACING_COMMON_H

#ifndef PATH_TRACING_CUSTOM_GLOBALS
#include "ShaderIncludes/PathTracing/Globals.h.slang"
#endif

static const float kRayTMax                    = 1.0e32;
static const float kInvPi                      = 0.31830988618;
static const float kRayOriginFloor             = 1.0 / 32.0;
static const float kRayOriginFloatEps          = 1.0 / 65536.0;
static const float kRayOriginIntScale          = 256.0;

#ifndef PATH_TRACING_PAYLOAD_EXTRA_FIELDS
#define PATH_TRACING_PAYLOAD_EXTRA_FIELDS
#endif

struct PathPayload
{
  float3 radiance;
  float3 throughput;
  float3 mediumAttenuation;
  float  mediumThickness;
  float3 lastSurfacePosition;
  float3 lastSurfaceNormal;
  float  lastDirectLightBsdfPdf;
  uint   seed;
  uint   depth;
  uint   mediumActive;
  uint   lastEnvironmentNeeActive;
  uint   hasLastBsdfSample;
  PATH_TRACING_PAYLOAD_EXTRA_FIELDS
};

struct ShadowPayload
{
  uint visible;
};

struct SurfaceData
{
  float3 worldPosition;
  float3 shadingNormal;
  float3 geometricNormal;
  float3 tangent;
  float3 bitangent;
  float2 texCoord;
  float3 albedo;
  float3 emission;
  float  metallic;
  float  roughness;
  float  specular;
  float  specularTint;
  float  subsurface;
  float  anisotropy;
  float3 attenuationColor;
  float  transmission;
  float  attenuationDistance;
  float  volumeThickness;
  float  refractionIndex;
  float  clearcoat;
  float  clearcoatRoughness;
  float3 sheenColor;
  float  sheenRoughness;
  uint   isFrontFace;
};

bool IsAccumulationEnabled()
{
  return (pushConst.flags & uint(PathTraceFlags::ePathTraceFlagAccumulate)) != 0;
}

bool IsFinalRadianceAccumulationEnabled()
{
  return IsAccumulationEnabled();
}
#endif









