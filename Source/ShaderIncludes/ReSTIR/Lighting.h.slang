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

// Adapter between the generic ReSTIR DI include code and this renderer's scene
// representation. The functions below are the light, surface, visibility, and
// BRDF hooks that the reservoir passes call from their shared HLSL code.

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

// Material resolution for light samples. ReSTIR compute passes do not run the
// closest-hit material loader, so the bridge resolves the small subset needed
// by emissive lights and direct-light target PDFs.
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

float DIEvaluateEmissiveTriangleCosine(GltfMetallicRoughness material, float3 geometricNormal, float3 directionToReceiver)
{
  // Double-sided emissive materials can illuminate both sides of the triangle.
  const float cosine = dot(normalize(geometricNormal), directionToReceiver);
  return material.doubleSided != 0 ? abs(cosine) : max(cosine, 0.0);
}

float3 DIEvaluateEmissiveTriangleRadiance(GltfSceneInfo sceneInfo, EmissiveTriangleLight light, float2 texCoord)
{
  // Emissive triangles use the same material alpha mask as visible geometry.
  const GltfMetallicRoughness material = sceneInfo.materials[light.materialIndex];
  const float4 baseColor = ResolveDIBaseColor(material, texCoord);
  if(material.alphaMode == GltfAlphaMode::eMask && baseColor.w < material.alphaCutoff)
  {
    return float3(0.0);
  }

  return ResolveDIEmission(material, texCoord);
}

float EvaluateEmissiveTriangleSolidAnglePdf(GltfSceneInfo sceneInfo,
                                            EmissiveTriangleLight light,
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
  const float lightCos = DIEvaluateEmissiveTriangleCosine(material, light.geometricNormal, -lightDirection);
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
  // Zero is the invalid primary-surface representation for all ReSTIR passes.
  return (ReSTIRDISurface)0;
}

ReSTIRDISurface PackDIPrimarySurface(SurfaceData surface, uint materialIndex, uint instanceIndex, uint primitiveIndex, float linearDepth)
{
  // Capture closest-hit material state once so later compute passes do not
  // need to repeat the full material and texture lookup.
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
  // Convert the packed DI surface back into the path-tracing surface shape so
  // the same Disney BRDF helpers can be reused.
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
  // ReSTIR treats sky, HDRI, and constant background as one environment light.
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
  // The generic include code sees one compact light range plus an optional environment entry.
  const uint localLightCount    = sceneInfo.emissiveTriangleCount;
  const uint environmentPresent = HasEnvironmentLight(sceneInfo) ? 1u : 0u;

  ReSTIRLightBufferParameters parameters = (ReSTIRLightBufferParameters)0;
  parameters.localLightBufferRegion.firstLightIndex    = 0u;
  parameters.localLightBufferRegion.numLights          = localLightCount;
  parameters.environmentLightParams.lightPresent       = environmentPresent;
  parameters.environmentLightParams.lightIndex         = localLightCount;
  return parameters;
}

float DIEvaluateLocalLightSelectionPdf(uint lightIndex)
{
  // Recover the discrete selection PDF from the CPU-built emissive CDF.
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  if(sceneInfo.emissiveTriangleCount == 0 || lightIndex >= sceneInfo.emissiveTriangleCount)
  {
    return 0.0;
  }

  const float previousCdf = lightIndex > 0 ? sceneInfo.emissiveTriangleCdf[lightIndex - 1] : 0.0;
  return max(sceneInfo.emissiveTriangleCdf[lightIndex] - previousCdf, 0.0);
}

DISurface DIEmptySurface()
{
  // Generic ReSTIR include code asks the renderer bridge for its empty surface.
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
  // Light info is only a compact reference; actual sampling happens later.
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
  // Spatial reuse is restricted to the same material in this renderer.
  return a == b;
}

int2 DIClampSamplePositionIntoView(int2 pixelPosition)
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
  // Out-of-view reprojection means there is no previous surface to reuse.
  const int width  = int(pushConst.sceneInfoAddress[0].viewportSize.x);
  const int height = int(pushConst.sceneInfoAddress[0].viewportSize.y);
  if(pixelPosition.x < 0 || pixelPosition.y < 0 || pixelPosition.x >= width || pixelPosition.y >= height)
  {
    return DIEmptySurface();
  }

  const uint pixelIndex = uint(pixelPosition.y) * uint(width) + uint(pixelPosition.x);
  return previousFrame ? previousSurfaceBuffer[pixelIndex] : currentSurfaceBuffer[pixelIndex];
}

bool IsSelfEmissiveSample(DISurface surface, EmissiveTriangleLight light)
{
  // Avoid sampling the triangle that the primary ray is already standing on.
  return surface.instanceIndex == light.instanceIndex && surface.primitiveIndex == light.primitiveIndex;
}

DILightInfo DILoadLightInfo(uint lightIndex)
{
  // Scene rebuilds invalidate temporal history, so previous-frame light
  // references map directly to the current light list.
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
  // Turn a compact reservoir sample reference into a concrete light sample at this surface.
  const GltfSceneInfo sceneInfo = pushConst.sceneInfoAddress[0];
  DILightSample sample = DIEmptyLightSample();
  sample.kind = lightInfo.kind;

  if(lightInfo.kind == kReSTIRDILightKindEmissiveTriangle)
  {
    // Emissive triangle candidates store barycentric random numbers in uv.
    const EmissiveTriangleLight light = sceneInfo.emissiveTriangles[lightInfo.sourceIndex];
    if(IsSelfEmissiveSample(surface, light))
    {
      return sample;
    }

    const float sqrtXi0 = sqrt(saturate(uv.x));
    const float xi1     = saturate(uv.y);
    const float3 bary   = float3(1.0 - sqrtXi0, sqrtXi0 * (1.0 - xi1), sqrtXi0 * xi1);

    sample.position      = light.position0 * bary.x + light.position1 * bary.y + light.position2 * bary.z;
    const float2 texCoord = light.texCoord0 * bary.x + light.texCoord1 * bary.y + light.texCoord2 * bary.z;
    sample.radiance      = DIEvaluateEmissiveTriangleRadiance(sceneInfo, light, texCoord);
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
    // Environment candidates store lat-long random numbers in uv.
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
  // Evaluate f*cos(theta) and the matching BRDF PDF for the direct-light target function.
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
  // Same BRDF evaluation as above, split into NRD diffuse and specular signals.
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
  // ReSTIR samples proportional to luminance of the visible direct-light contribution.
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
  // BRDF candidates let the initial pass discover lights that explicit light sampling may miss.
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
    // Broad lobes use cosine sampling as the diffuse/subsurface candidate direction.
    float ignoredPdf = 0.0;
    sampleDir = SampleCosineHemisphereDirection(unpackedSurface.shadingNormal, xi, ignoredPdf);
  }
  else if(sampleGroup < broadProbability + reflectionProbability)
  {
    // Reflection lobes sample visible GGX for glossy candidates.
    const float3 viewLocal = ToLocal(unpackedSurface.shadingNormal, unpackedSurface.tangent, unpackedSurface.bitangent, viewDirection);
    const float3 halfLocal = ImportanceSampleVisibleGGX(viewLocal, material.alphaAnisotropicX, material.alphaAnisotropicY, xi);
    const float3 halfVector = ToWorld(unpackedSurface.shadingNormal, unpackedSurface.tangent, unpackedSurface.bitangent, halfLocal);
    sampleDir = normalize(reflect(-viewDirection, halfVector));
  }
  else
  {
    // Clearcoat has its own sharper GGX lobe.
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
  // Trace a BRDF candidate and translate the hit triangle back into a ReSTIR light id.
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
    const EmissiveTriangleLight light = sceneInfo.emissiveTriangles[i];
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
  return DIEvaluateLocalLightSelectionPdf(lightIndex);
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
  // Final visibility is a shadow ray from the receiver toward the selected light sample.
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

bool DIGetTemporalConservativeVisibility(DISurface surface, DILightSample lightSample)
{
  // Temporal reuse samples a previous-frame surface, but this renderer
  // keeps one current-frame TLAS. The visibility ray is therefore an
  // approximation from the current receiver.
  return EvaluateLightSampleVisibility(surface, lightSample);
}

int DITranslateLightIndex(uint lightIndex)
{
  // Scene rebuilds invalidate temporal history, so previous-frame light
  // references map directly to the current light list.
  return lightIndex < GetTotalSupportedLightCount(pushConst.sceneInfoAddress[0]) ? int(lightIndex) : -1;
}

void EvaluateLightSampleContributionSplit(DISurface surface, DILightSample lightSample, out float3 diffuseContribution, out float3 specularContribution)
{
  // Convert the selected light sample into raw diffuse/specular radiance before reservoir weight.
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
  // Miss pixels still need background radiance for the final image.
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
  // Resolve one selected reservoir into final radiance and NRD-compatible signal splits.
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

  const DILightInfo   lightInfo   = DILoadLightInfo(GetDIReservoirLightIndex(reservoir));
  const DILightSample lightSample = DISampleLight(lightInfo, surface, GetDIReservoirSampleUv(reservoir));
  if(lightSample.valid == 0 || lightSample.solidAnglePdf <= 0.0 || isinf(lightSample.solidAnglePdf) || isnan(lightSample.solidAnglePdf))
  {
    return resolved;
  }

  float3 visibility = float3(1.0);
  if(diParams.shading.enableFinalVisibility != 0)
  {
    // Reuse cached final visibility when it is still close enough in time and screen space.
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
      // Fresh final visibility is the expensive shadow ray that validates the selected sample.
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

  // Reservoir inverse PDF and light solid-angle PDF together form the final sample weight.
  const float lightWeight = GetDIReservoirInvPdf(reservoir) / lightSample.solidAnglePdf;
  diffuseContribution *= visibility * lightWeight;
  specularContribution *= visibility * lightWeight;

  resolved.diffuseRadiance += diffuseContribution;
  resolved.specularRadiance = specularContribution;
  resolved.totalRadiance    = resolved.diffuseRadiance + resolved.specularRadiance;

  // NRD needs a hit distance for each noisy signal it denoises.
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
