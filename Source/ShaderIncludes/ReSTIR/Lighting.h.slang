#ifndef RESTIR_DI_LIGHTING_H
#define RESTIR_DI_LIGHTING_H

#ifndef RESTIR_ALLOWED_BIAS_CORRECTION
#define RESTIR_ALLOWED_BIAS_CORRECTION RESTIR_BIAS_CORRECTION_RAY_TRACED
#endif

#define RESTIR_LIGHT_RESERVOIR_BUFFER diReservoirBuffer
#define RESTIR_NEIGHBOR_OFFSETS_BUFFER diNeighborOffsetBuffer

#include "ShaderIncludes/Camera.h.slang"
#include "ShaderIncludes/PathTracing/Common.h.slang"
#include "ShaderIncludes/PathTracing/Utility.h.slang"
#include "ShaderIncludes/PathTracing/MonteCarlo.h.slang"
#include "ShaderIncludes/PathTracing/Hdri.h.slang"
#include "ShaderIncludes/PathTracing/Disney.h.slang"

#include "ReSTIR/Random.hlsli"
#include "ReSTIR/Reservoir.hlsli"
#include "ReSTIR/ReservoirStorage.hlsli"

static const uint kReSTIRDILightKindInvalid          = 0u;
static const uint kReSTIRDILightKindEmissiveTriangle = 1u;
static const uint kReSTIRDILightKindEnvironment      = 2u;
static const float kDIDenoiserMissHitDistance        = 65504.0;

typedef ReSTIRDISurface DISurface;
typedef uint DIMaterial;

struct DILightInfo
{
  uint kind;
  uint sourceIndex;
  uint pad0;
  uint pad1;
};

struct DILightSample
{
  float3 position;
  float  distance;
  float3 direction;
  float  solidAnglePdf;
  float3 radiance;
  uint   kind;
  float3 normal;
  uint   valid;
};

struct DIResolvedDirectLighting
{
  float3 totalRadiance;
  float  diffuseHitDistance;
  float3 diffuseRadiance;
  float  specularHitDistance;
  float3 specularRadiance;
  float  pad0;
};

float4 ResolveDIBaseColor(GltfMetallicRoughness material, float2 texCoord)
{
  float4 baseColor = material.baseColorFactor;
  if(material.baseColorTextureIndex >= 0)
  {
    baseColor *= textures[material.baseColorTextureIndex].SampleLevel(texCoord, 0.0);
  }
  return baseColor;
}

float3 ResolveDIEmission(GltfMetallicRoughness material, float2 texCoord)
{
  float3 emission = max(material.emissionFactor, float3(0.0));
  if(material.emissiveTextureIndex >= 0)
  {
    emission *= textures[material.emissiveTextureIndex].SampleLevel(texCoord, 0.0).xyz;
  }
  return max(emission, float3(0.0));
}

#ifndef PATH_TRACING_LIGHTS_H
float EvaluateEmissiveTriangleCosine(GltfMetallicRoughness material, float3 geometricNormal, float3 directionToReceiver)
{
  const float cosine = dot(normalize(geometricNormal), directionToReceiver);
  return material.doubleSided != 0 ? abs(cosine) : max(cosine, 0.0);
}

float3 EvaluateEmissiveTriangleRadiance(GltfSceneInfo sceneInfo, PathTraceEmissiveTriangle light, float2 texCoord)
{
  const GltfMetallicRoughness material = sceneInfo.materials[light.materialIndex];
  const float4 baseColor = ResolveDIBaseColor(material, texCoord);
  if(material.alphaMode == GltfAlphaMode::eMask && baseColor.w < material.alphaCutoff)
  {
    return float3(0.0);
  }

  return ResolveDIEmission(material, texCoord);
}
#endif

float EvaluateEmissiveTriangleSolidAnglePdf(GltfSceneInfo sceneInfo,
                                            PathTraceEmissiveTriangle light,
                                            float3 receiverPosition,
                                            float3 sampledPosition)
{
  if(light.area <= 1.0e-6)
  {
    return 0.0;
  }

  const float3 toLight = sampledPosition - receiverPosition;
  const float distanceSquared = dot(toLight, toLight);
  if(distanceSquared <= 1.0e-8)
  {
    return 0.0;
  }

  const GltfMetallicRoughness material = sceneInfo.materials[light.materialIndex];
  const float3 lightDirection = toLight * rsqrt(distanceSquared);
  const float lightCos = EvaluateEmissiveTriangleCosine(material, light.geometricNormal, -lightDirection);
  if(lightCos <= 0.0)
  {
    return 0.0;
  }

  // The DI estimator passes the discrete light-selection PDF separately via invSourcePdf.
  // This function must return only the conditional directional PDF for a
  // sample on the already-selected emissive triangle.
  const float areaPdf = 1.0 / light.area;
  return areaPdf * distanceSquared / max(lightCos, 1.0e-6);
}

ReSTIRDISurface EmptyDIPrimarySurface()
{
  return (ReSTIRDISurface)0;
}

ReSTIRDISurface PackDIPrimarySurface(SurfaceData surface, uint materialIndex, uint instanceIndex, uint primitiveIndex, float linearDepth)
{
  ReSTIRDISurface packed = EmptyDIPrimarySurface();
  packed.worldPosition      = surface.worldPosition;
  packed.linearDepth        = linearDepth;
  packed.shadingNormal      = surface.shadingNormal;
  packed.roughness          = surface.roughness;
  packed.geometricNormal    = surface.geometricNormal;
  packed.metallic           = surface.metallic;
  packed.tangent            = surface.tangent;
  packed.specular           = surface.specular;
  packed.bitangent          = surface.bitangent;
  packed.specularTint       = surface.specularTint;
  packed.albedo             = surface.albedo;
  packed.transmission       = surface.transmission;
  packed.emission           = surface.emission;
  packed.attenuationDistance = surface.attenuationDistance;
  packed.attenuationColor   = surface.attenuationColor;
  packed.volumeThickness    = surface.volumeThickness;
  packed.sheenColor         = surface.sheenColor;
  packed.refractionIndex    = surface.refractionIndex;
  packed.clearcoat          = surface.clearcoat;
  packed.clearcoatRoughness = surface.clearcoatRoughness;
  packed.sheenRoughness     = surface.sheenRoughness;
  packed.subsurface         = surface.subsurface;
  packed.anisotropy         = surface.anisotropy;
  packed.materialIndex      = materialIndex;
  packed.instanceIndex      = instanceIndex;
  packed.primitiveIndex     = primitiveIndex;
  packed.isFrontFace        = surface.isFrontFace;
  packed.valid              = 1u;
  return packed;
}

SurfaceData UnpackDIPrimarySurface(ReSTIRDISurface surface)
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

bool HasEnvironmentLight(GltfSceneInfo sceneInfo)
{
  return (sceneInfo.useSky != 0) || (sceneInfo.useHdrEnv != 0) || SafeMax3(sceneInfo.backgroundColor) > 0.0;
}

uint GetTotalSupportedLightCount(GltfSceneInfo sceneInfo)
{
  // Keep DI aligned with the current ground-truth tracer contract: emissive
  // triangles + environment. Punctual lights are not part of that reference.
  return sceneInfo.emissiveTriangleCount + (HasEnvironmentLight(sceneInfo) ? 1u : 0u);
}

ReSTIRLightBufferParameters BuildLightBufferParameters(GltfSceneInfo sceneInfo)
{
  const uint localLightCount    = sceneInfo.emissiveTriangleCount;
  const uint environmentPresent = HasEnvironmentLight(sceneInfo) ? 1u : 0u;

  ReSTIRLightBufferParameters parameters = (ReSTIRLightBufferParameters)0;
  parameters.localLightBufferRegion.firstLightIndex    = 0u;
  parameters.localLightBufferRegion.numLights          = localLightCount;
  parameters.environmentLightParams.lightPresent       = environmentPresent;
  parameters.environmentLightParams.lightIndex         = localLightCount;
  return parameters;
}

DISurface DIEmptySurface()
{
  return EmptyDIPrimarySurface();
}

DILightInfo DIEmptyLightInfo()
{
  return (DILightInfo)0;
}

DILightSample DIEmptyLightSample()
{
  return (DILightSample)0;
}

DILightInfo MakeLightInfo(uint kind, uint sourceIndex)
{
  DILightInfo info = DIEmptyLightInfo();
  info.kind          = kind;
  info.sourceIndex   = sourceIndex;
  return info;
}

bool DIIsSurfaceValid(DISurface surface)
{
  return surface.valid != 0;
}

float3 DIGetSurfaceNormal(DISurface surface)
{
  return surface.shadingNormal;
}

float DIGetSurfaceLinearDepth(DISurface surface)
{
  return surface.linearDepth;
}

float3 DIGetSurfaceWorldPosition(DISurface surface)
{
  return surface.worldPosition;
}

float3 DIGetSurfaceGeometricNormal(DISurface surface)
{
  return surface.geometricNormal;
}

DIMaterial DIGetMaterial(DISurface surface)
{
  return surface.materialIndex;
}

bool DIAreMaterialsSimilar(DIMaterial a, DIMaterial b)
{
  return a == b;
}

int2 DIClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
  const int width  = int(pushConst.sceneInfoAddress[0].viewportSize.x);
  const int height = int(pushConst.sceneInfoAddress[0].viewportSize.y);
  if(width <= 0 || height <= 0)
  {
    return int2(0, 0);
  }

  // Reflect across screen edges instead of hard clamping to avoid repeatedly
  // collapsing many out-of-bounds neighbors onto the same boundary texels.
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

DISurface DILoadGBufferSurface(int2 pixelPosition, bool previousFrame)
{
  const int width  = int(pushConst.sceneInfoAddress[0].viewportSize.x);
  const int height = int(pushConst.sceneInfoAddress[0].viewportSize.y);
  if(pixelPosition.x < 0 || pixelPosition.y < 0 || pixelPosition.x >= width || pixelPosition.y >= height)
  {
    return DIEmptySurface();
  }

  const uint pixelIndex = uint(pixelPosition.y) * uint(width) + uint(pixelPosition.x);
  return previousFrame ? previousSurfaceBuffer[pixelIndex] : currentSurfaceBuffer[pixelIndex];
}

bool IsSelfEmissiveSample(DISurface surface, PathTraceEmissiveTriangle light)
{
  return surface.instanceIndex == light.instanceIndex && surface.primitiveIndex == light.primitiveIndex;
}

DILightInfo DILoadLightInfo(uint lightIndex, bool previousFrame)
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  const uint localLightCount    = sceneInfo.emissiveTriangleCount;
  const uint environmentPresent = HasEnvironmentLight(sceneInfo) ? 1u : 0u;

  if(lightIndex < localLightCount)
  {
    return MakeLightInfo(kReSTIRDILightKindEmissiveTriangle, lightIndex);
  }

  if(environmentPresent != 0 && lightIndex == localLightCount)
  {
    return MakeLightInfo(kReSTIRDILightKindEnvironment, 0u);
  }

  return MakeLightInfo(kReSTIRDILightKindInvalid, 0u);
}

DILightSample DISampleLight(DILightInfo lightInfo, DISurface surface, float2 uv)
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  DILightSample sample = DIEmptyLightSample();
  sample.kind = lightInfo.kind;

  if(lightInfo.kind == kReSTIRDILightKindEmissiveTriangle)
  {
    const PathTraceEmissiveTriangle light = sceneInfo.emissiveTriangles[lightInfo.sourceIndex];
    if(IsSelfEmissiveSample(surface, light))
    {
      return sample;
    }

    const float sqrtXi0 = sqrt(saturate(uv.x));
    const float xi1     = saturate(uv.y);
    const float3 bary   = float3(1.0 - sqrtXi0, sqrtXi0 * (1.0 - xi1), sqrtXi0 * xi1);

    sample.position      = light.position0 * bary.x + light.position1 * bary.y + light.position2 * bary.z;
    const float2 texCoord = light.texCoord0 * bary.x + light.texCoord1 * bary.y + light.texCoord2 * bary.z;
    sample.radiance      = EvaluateEmissiveTriangleRadiance(sceneInfo, light, texCoord);
    const float3 toLight = sample.position - surface.worldPosition;
    const float distanceSquared = dot(toLight, toLight);
    if(distanceSquared <= 1.0e-8 || SafeMax3(sample.radiance) <= 0.0)
    {
      return DIEmptyLightSample();
    }

    sample.distance      = sqrt(distanceSquared);
    sample.direction     = toLight / sample.distance;
    sample.normal        = light.geometricNormal;
    sample.solidAnglePdf = EvaluateEmissiveTriangleSolidAnglePdf(sceneInfo, light, surface.worldPosition, sample.position);
    sample.valid         = sample.solidAnglePdf > 0.0 ? 1u : 0u;
    return sample;
  }

  if(lightInfo.kind == kReSTIRDILightKindEnvironment)
  {
    sample.direction     = LatLongUvToDir(uv);
    sample.distance      = kRayTMax;
    sample.position      = surface.worldPosition + sample.direction * sample.distance;
    sample.radiance      = SampleEnvironment(sceneInfo, sample.direction);
    sample.normal        = -sample.direction;
    sample.solidAnglePdf = EvaluateEnvironmentLightPdf(sceneInfo, surface.shadingNormal, sample.direction);
    sample.valid         = (sample.solidAnglePdf > 0.0 && SafeMax3(sample.radiance) > 0.0) ? 1u : 0u;
  }

  return sample;
}

void DIGetLightDirectionAndDistance(DISurface surface, DILightSample lightSample, out float3 lightDir, out float lightDistance)
{
  lightDir      = lightSample.direction;
  lightDistance = lightSample.distance;
}

bool DIIsAnalyticLightSample(DILightSample lightSample)
{
  return false;
}

float DIGetLightSampleSolidAnglePdf(DILightSample lightSample)
{
  return lightSample.solidAnglePdf;
}

float3 EvaluateDIBrdfContribution(DISurface surface, float3 lightDir, out float pdf)
{
  const SurfaceData unpackedSurface = UnpackDIPrimarySurface(surface);
  const float3 viewDirection        = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surface.worldPosition);

  const DisneyMaterialData material = GetDisneyMaterialData(unpackedSurface);
  const DisneyBrdfData     brdf     = PrepareBrdfData(unpackedSurface, material, viewDirection, lightDir);
  const DisneyLobeSetup    setup    = GetLobeSetup(material, brdf);

  const float broadWeight      = BroadGroupProbability(setup);
  const float reflectionWeight = ReflectionGroupProbability(setup);
  const float clearcoatWeight  = setup.clearcoatProbability;
  const float totalWeight      = broadWeight + reflectionWeight + clearcoatWeight;
  if(totalWeight <= 0.0)
  {
    pdf = 0.0;
    return float3(0.0);
  }

  float broadPdf      = 0.0;
  float reflectionPdf = 0.0;
  float clearcoatPdf  = 0.0;

  const float3 broadContribution      = EvaluateBroadBsdfGroup(material, brdf, setup, broadPdf);
  const float3 reflectionContribution = EvaluateReflectionBsdfGroup(material, brdf, setup, reflectionPdf);
  const float3 clearcoatContribution  = EvaluateClearcoatBsdfGroup(material, brdf, setup, clearcoatPdf);

  pdf = broadPdf * (broadWeight / totalWeight) + reflectionPdf * (reflectionWeight / totalWeight)
        + clearcoatPdf * (clearcoatWeight / totalWeight);

  return max(broadContribution + reflectionContribution + clearcoatContribution, float3(0.0));
}

void EvaluateDIBrdfContributionSplit(DISurface surface, float3 lightDir, out float3 diffuseContribution, out float3 specularContribution, out float pdf)
{
  diffuseContribution  = float3(0.0);
  specularContribution = float3(0.0);
  pdf                  = 0.0;

  const SurfaceData unpackedSurface = UnpackDIPrimarySurface(surface);
  const float3 viewDirection        = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surface.worldPosition);

  const DisneyMaterialData material = GetDisneyMaterialData(unpackedSurface);
  const DisneyBrdfData     brdf     = PrepareBrdfData(unpackedSurface, material, viewDirection, lightDir);
  const DisneyLobeSetup    setup    = GetLobeSetup(material, brdf);

  const float broadWeight      = BroadGroupProbability(setup);
  const float reflectionWeight = ReflectionGroupProbability(setup);
  const float clearcoatWeight  = setup.clearcoatProbability;
  const float totalWeight      = broadWeight + reflectionWeight + clearcoatWeight;
  if(totalWeight <= 0.0)
  {
    return;
  }

  float broadPdf      = 0.0;
  float reflectionPdf = 0.0;
  float clearcoatPdf  = 0.0;

  diffuseContribution = EvaluateBroadBsdfGroup(material, brdf, setup, broadPdf);
  const float3 reflectionContribution = EvaluateReflectionBsdfGroup(material, brdf, setup, reflectionPdf);
  const float3 clearcoatContribution  = EvaluateClearcoatBsdfGroup(material, brdf, setup, clearcoatPdf);
  specularContribution                = reflectionContribution + clearcoatContribution;

  pdf = broadPdf * (broadWeight / totalWeight) + reflectionPdf * (reflectionWeight / totalWeight)
        + clearcoatPdf * (clearcoatWeight / totalWeight);

  diffuseContribution  = max(diffuseContribution, float3(0.0));
  specularContribution = max(specularContribution, float3(0.0));
}

float DIGetLightSampleTargetPdf(DILightSample lightSample, DISurface surface)
{
  if(lightSample.valid == 0 || lightSample.solidAnglePdf <= 0.0 || isinf(lightSample.solidAnglePdf) || isnan(lightSample.solidAnglePdf))
  {
    return 0.0;
  }

  float bsdfPdf        = 0.0;
  const float3 brdfCos = EvaluateDIBrdfContribution(surface, lightSample.direction, bsdfPdf);
  return Luminance(max(brdfCos * lightSample.radiance, float3(0.0))) / lightSample.solidAnglePdf;
}

float DIEvaluateSurfaceBrdfPdf(DISurface surface, float3 lightDir)
{
  float pdf                         = 0.0;
  EvaluateDIBrdfContribution(surface, lightDir, pdf);
  return pdf;
}

bool DISampleSurfaceBrdf(DISurface surface, inout ReSTIRRandomSamplerState rng, out float3 sampleDir)
{
  sampleDir = float3(0.0);

  const SurfaceData unpackedSurface = UnpackDIPrimarySurface(surface);
  const float3 viewDirection        = normalize(pushConst.sceneInfoAddress[0].cameraPosition - surface.worldPosition);

  const DisneyMaterialData material  = GetDisneyMaterialData(unpackedSurface);
  const DisneyBrdfData     probeBrdf = PrepareBrdfData(unpackedSurface, material, viewDirection, unpackedSurface.shadingNormal);
  const DisneyLobeSetup    setup     = GetLobeSetup(material, probeBrdf);

  const float broadWeight      = BroadGroupProbability(setup);
  const float reflectionWeight = ReflectionGroupProbability(setup);
  const float clearcoatWeight  = setup.clearcoatProbability;
  const float totalWeight      = broadWeight + reflectionWeight + clearcoatWeight;
  if(totalWeight <= 0.0)
  {
    return false;
  }

  const float broadProbability      = broadWeight / totalWeight;
  const float reflectionProbability = reflectionWeight / totalWeight;
  const float sampleGroup           = GetNextRandom(rng);
  const float2 xi                   = float2(GetNextRandom(rng), GetNextRandom(rng));

  if(sampleGroup < broadProbability)
  {
    float ignoredPdf = 0.0;
    sampleDir = SampleCosineHemisphereDirection(unpackedSurface.shadingNormal, xi, ignoredPdf);
  }
  else if(sampleGroup < broadProbability + reflectionProbability)
  {
    const float3 viewLocal = ToLocal(unpackedSurface.shadingNormal, unpackedSurface.tangent, unpackedSurface.bitangent, viewDirection);
    const float3 halfLocal = ImportanceSampleVisibleGGX(viewLocal, material.alphaAnisotropicX, material.alphaAnisotropicY, xi);
    const float3 halfVector = ToWorld(unpackedSurface.shadingNormal, unpackedSurface.tangent, unpackedSurface.bitangent, halfLocal);
    sampleDir = normalize(reflect(-viewDirection, halfVector));
  }
  else
  {
    float3 halfLocal = ImportanceSampleGGX(material.clearcoatAlpha, xi);
    if(halfLocal.z < 0.0)
    {
      halfLocal = -halfLocal;
    }
    const float3 halfVector = ToWorld(unpackedSurface.shadingNormal, unpackedSurface.tangent, unpackedSurface.bitangent, halfLocal);
    sampleDir = normalize(reflect(-viewDirection, halfVector));
  }

  if(dot(sampleDir, sampleDir) <= 0.0)
  {
    return false;
  }

  if(dot(unpackedSurface.geometricNormal, sampleDir) <= 0.0)
  {
    return false;
  }

  float samplePdf = 0.0;
  const float3 contribution = EvaluateDIBrdfContribution(surface, sampleDir, samplePdf);
  return samplePdf > 0.0 && SafeMax3(contribution) > 0.0;
}

bool DITraceRayForLocalLight(float3 origin, float3 direction, float minT, float maxT, out uint lightIndex, out float2 randXY)
{
  lightIndex = ReSTIRInvalidLightIndex;
  randXY     = float2(0.0);
  if(maxT <= minT)
  {
    return false;
  }

  RayDesc ray;
  ray.Origin    = origin;
  ray.Direction = direction;
  ray.TMin      = minT;
  ray.TMax      = maxT;

  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(topLevelAS, RAY_FLAG_NONE, 0xFF, ray);

  while(query.Proceed())
  {
    if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
    {
      query.CommitNonOpaqueTriangleHit();
    }
  }

  if(query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
  {
    return false;
  }

  const uint committedInstance  = query.CommittedInstanceID();
  const uint committedPrimitive = query.CommittedPrimitiveIndex();
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  for(uint i = 0; i < sceneInfo.emissiveTriangleCount; ++i)
  {
    const PathTraceEmissiveTriangle light = sceneInfo.emissiveTriangles[i];
    if(light.instanceIndex == committedInstance && light.primitiveIndex == committedPrimitive)
    {
      lightIndex = i;

      // Inverse of the barycentric mapping used in DISampleLight
      // for emissive triangles, so BRDF-hit samples reuse the exact hit point.
      const float2 hitUv      = query.CommittedTriangleBarycentrics();
      const float3 bary       = float3(1.0 - hitUv.x - hitUv.y, hitUv.x, hitUv.y);
      const float  sqrtXi0    = saturate(1.0 - bary.x);
      const float  denom      = max(bary.y + bary.z, 1.0e-6);
      randXY.x                = saturate(sqrtXi0 * sqrtXi0);
      randXY.y                = saturate(bary.z / denom);
      return true;
    }
  }

  // Hit geometry that is not in the local emissive light list.
  return true;
}

float DIEvaluateLocalLightSourcePdf(uint lightIndex)
{
  const uint lightCount = BuildLightBufferParameters(pushConst.sceneInfoAddress[0]).localLightBufferRegion.numLights;
  return (lightIndex < lightCount && lightCount > 0) ? (1.0 / lightCount) : 0.0;
}

float2 DIGetEnvironmentMapUvFromDirection(float3 direction)
{
  return dirToLatLongUv(direction);
}

float DIEvaluateEnvironmentMapSamplingPdf(float3 direction)
{
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  if(!HasEnvironmentLight(sceneInfo))
  {
    return 0.0;
  }

  // The canonical explicit environment sampler in this DI bridge returns the
  // full solid-angle PDF through DIGetLightSampleSolidAnglePdf(...), so the
  // remaining "selection pdf" term is 1.
  return 1.0;
}

bool TraceDIVisibility(float3 worldPosition, float3 geometricNormal, float3 direction, float maxDistance)
{
  if(maxDistance <= 1.0e-4)
  {
    return true;
  }

  RayDesc ray;
  ray.Origin    = OffsetRay(worldPosition, SelectOffsetNormal(geometricNormal, direction));
  ray.Direction = direction;
  ray.TMin      = 0.0;
  ray.TMax      = maxDistance;

  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(topLevelAS, RAY_FLAG_NONE, 0xFF, ray);

  while(query.Proceed())
  {
    if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
    {
      query.CommitNonOpaqueTriangleHit();
    }
  }

  return query.CommittedStatus() == COMMITTED_NOTHING;
}

bool EvaluateLightSampleVisibility(DISurface surface, DILightSample lightSample)
{
  if(lightSample.valid == 0)
  {
    return false;
  }

  const float maxDistance = max(lightSample.distance - 0.001, 0.001);
  return TraceDIVisibility(surface.worldPosition, surface.geometricNormal, lightSample.direction, maxDistance);
}

bool DIGetConservativeVisibility(DISurface surface, DILightSample lightSample)
{
  return EvaluateLightSampleVisibility(surface, lightSample);
}

bool DIGetTemporalConservativeVisibility(DISurface surface, DISurface temporalSurface, DILightSample lightSample)
{
  return EvaluateLightSampleVisibility(surface, lightSample);
}

int DITranslateLightIndex(uint lightIndex, bool previousFrame)
{
  return lightIndex < GetTotalSupportedLightCount(pushConst.sceneInfoAddress[0]) ? int(lightIndex) : -1;
}

void EvaluateLightSampleContributionSplit(DISurface surface, DILightSample lightSample, out float3 diffuseContribution, out float3 specularContribution)
{
  diffuseContribution  = float3(0.0);
  specularContribution = float3(0.0);
  if(lightSample.valid == 0)
  {
    return;
  }

  float  bsdfPdf                = 0.0;
  float3 diffuseBrdfContribution = float3(0.0);
  float3 specularBrdfContribution = float3(0.0);
  EvaluateDIBrdfContributionSplit(surface, lightSample.direction, diffuseBrdfContribution, specularBrdfContribution, bsdfPdf);
  diffuseContribution  = max(diffuseBrdfContribution * lightSample.radiance, float3(0.0));
  specularContribution = max(specularBrdfContribution * lightSample.radiance, float3(0.0));
}

float3 ReconstructMissRadiance(uint2 pixelPosition)
{
  const GltfSceneInfo sceneInfo   = pushConst.sceneInfoAddress[0];
  const float2 pixelCenter        = float2(pixelPosition) + 0.5;
  const float3 worldDirection     = ReconstructWorldDirectionFromPixel(pixelCenter, sceneInfo);
  return SampleEnvironment(sceneInfo, worldDirection);
}

DIResolvedDirectLighting ResolveReservoirDirectLightingSplit(uint2 pixelPosition,
                                                             DISurface surface,
                                                             ReSTIRDIReservoir reservoir,
                                                             bool updateVisibility)
{
  DIResolvedDirectLighting resolved = (DIResolvedDirectLighting)0;

  if(!DIIsSurfaceValid(surface))
  {
    resolved.totalRadiance = ReconstructMissRadiance(pixelPosition);
    return resolved;
  }

  resolved.totalRadiance   = surface.emission;
  resolved.diffuseRadiance = surface.emission;

  if(!IsValidDIReservoir(reservoir))
  {
    return resolved;
  }

  const DILightInfo   lightInfo   = DILoadLightInfo(GetDIReservoirLightIndex(reservoir), false);
  const DILightSample lightSample = DISampleLight(lightInfo, surface, GetDIReservoirSampleUv(reservoir));
  if(lightSample.valid == 0 || lightSample.solidAnglePdf <= 0.0 || isinf(lightSample.solidAnglePdf) || isnan(lightSample.solidAnglePdf))
  {
    return resolved;
  }

  float3 visibility = float3(1.0);
  if(diParams.shading.enableFinalVisibility != 0)
  {
    bool reusedVisibility = false;
    if(diParams.shading.reuseFinalVisibility != 0)
    {
      ReSTIRDIVisibilityReuseParameters reuseParameters;
      reuseParameters.maxAge      = diParams.shading.finalVisibilityMaxAge;
      reuseParameters.maxDistance = diParams.shading.finalVisibilityMaxDistance;
      reusedVisibility = GetDIReservoirVisibility(reservoir, reuseParameters, visibility);
    }

    if(!reusedVisibility)
    {
      visibility = EvaluateLightSampleVisibility(surface, lightSample) ? float3(1.0) : float3(0.0);
      if(updateVisibility)
      {
        StoreVisibilityInDIReservoir(reservoir, visibility, false);
        StoreDIReservoir(reservoir, diParams.reservoirBufferParams,
                         PixelPosToReservoirPos(pixelPosition),
                         diParams.bufferIndices.shadingInputBufferIndex);
      }
    }
  }

  float3 diffuseContribution  = float3(0.0);
  float3 specularContribution = float3(0.0);
  EvaluateLightSampleContributionSplit(surface, lightSample, diffuseContribution, specularContribution);

  const float lightWeight = GetDIReservoirInvPdf(reservoir) / lightSample.solidAnglePdf;
  diffuseContribution *= visibility * lightWeight;
  specularContribution *= visibility * lightWeight;

  resolved.diffuseRadiance += diffuseContribution;
  resolved.specularRadiance = specularContribution;
  resolved.totalRadiance    = resolved.diffuseRadiance + resolved.specularRadiance;

  const float lightHitDistance =
      (lightInfo.kind == kReSTIRDILightKindEnvironment) ? kDIDenoiserMissHitDistance : max(lightSample.distance, 0.0);
  if(SafeMax3(diffuseContribution) > 0.0)
  {
    resolved.diffuseHitDistance = lightHitDistance;
  }
  if(SafeMax3(specularContribution) > 0.0)
  {
    resolved.specularHitDistance = lightHitDistance;
  }

  return resolved;
}

#endif
